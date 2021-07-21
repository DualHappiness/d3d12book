//***************************************************************************************
// ShapesApp.cpp by Frank Luna (C) 2015 All Rights Reserved.
//
// Hold down '1' key to view scene in wireframe mode.
//***************************************************************************************

#include "../../Common/GeometryGenerator.h"
#include "../../Common/MathHelper.h"
#include "../../Common/UploadBuffer.h"
#include "../../Common/d3dApp.h"
#include "FrameResource.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;

const int gNumFrameResources = 3;

// Lightweight structure stores parameters to draw a shape.  This will
// vary from app-to-app.
struct RenderItem {
  RenderItem() = default;

  // World matrix of the shape that describes the object's local space
  // relative to the world space, which defines the position, orientation,
  // and scale of the object in the world.
  XMFLOAT4X4 World = MathHelper::Identity4x4();

  XMFLOAT4X4 TexTransform = MathHelper::Identity4x4();
  // Dirty flag indicating the object data has changed and we need to update the
  // constant buffer. Because we have an object cbuffer for each FrameResource,
  // we have to apply the update to each FrameResource.  Thus, when we modify
  // obect data we should set NumFramesDirty = gNumFrameResources so that each
  // frame resource gets the update.
  int NumFramesDirty = gNumFrameResources;

  // Index into GPU constant buffer corresponding to the ObjectCB for this
  // render item.
  UINT ObjCBIndex = -1;

  Material *Mat = nullptr;
  MeshGeometry *Geo = nullptr;

  // Primitive topology.
  D3D12_PRIMITIVE_TOPOLOGY PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

  // DrawIndexedInstanced parameters.
  UINT IndexCount = 0;
  UINT StartIndexLocation = 0;
  int BaseVertexLocation = 0;
};

class ShapesApp : public D3DApp {
public:
  ShapesApp(HINSTANCE hInstance);
  ShapesApp(const ShapesApp &rhs) = delete;
  ShapesApp &operator=(const ShapesApp &rhs) = delete;
  ~ShapesApp();

  virtual bool Initialize() override;

private:
  virtual void OnResize() override;
  virtual void Update(const GameTimer &gt) override;
  virtual void Draw(const GameTimer &gt) override;

  virtual void OnMouseDown(WPARAM btnState, int x, int y) override;
  virtual void OnMouseUp(WPARAM btnState, int x, int y) override;
  virtual void OnMouseMove(WPARAM btnState, int x, int y) override;

  void OnKeyboardInput(const GameTimer &gt);
  void UpdateCamera(const GameTimer &gt);
  void UpdateObjectCBs(const GameTimer &gt);
  void UpdateMainPassCB(const GameTimer &gt);
  void UpdateMaterialCBs(const GameTimer &gt);

  void BuildDescriptorHeaps();
  void BuildConstantBufferViews();
  void BuildRootSignature();
  void BuildShadersAndInputLayout();
  void BuildShapeGeometry();
  void BuildMaterials();
  void BuildPSOs();
  void BuildFrameResources();
  void BuildRenderItems();
  void DrawRenderItems(ID3D12GraphicsCommandList *cmdList,
                       const std::vector<RenderItem *> &ritems);

private:
  std::vector<std::unique_ptr<FrameResource>> mFrameResources;
  FrameResource *mCurrFrameResource = nullptr;
  int mCurrFrameResourceIndex = 0;

  UINT mCbvSrvDesciptorSize = 0;
  ComPtr<ID3D12RootSignature> mRootSignature = nullptr;

  std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;
  std::unordered_map<std::string, std::unique_ptr<Material>> mMaterials;
  std::unordered_map<std::string, std::unique_ptr<Texture>> mTxtures;
  std::unordered_map<std::string, ComPtr<ID3DBlob>> mShaders;
  std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> mPSOs;

  std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;

  // List of all the render items.
  std::vector<std::unique_ptr<RenderItem>> mAllRitems;

  // Render items divided by PSO.
  std::vector<RenderItem *> mOpaqueRitems;

  PassConstants mMainPassCB;

  UINT mPassCbvOffset = 0;

  bool mIsWireframe = false;

  XMFLOAT3 mEyePos = {0.0f, 0.0f, 0.0f};
  XMFLOAT4X4 mView = MathHelper::Identity4x4();
  XMFLOAT4X4 mProj = MathHelper::Identity4x4();

  float mTheta = 1.5f * XM_PI;
  float mPhi = 0.2f * XM_PI;
  float mRadius = 15.0f;

  POINT mLastMousePos;
};

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE prevInstance, PSTR cmdLine,
                   int showCmd) {
  // Enable run-time memory check for debug builds.
#if defined(DEBUG) | defined(_DEBUG)
  _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

  try {
    ShapesApp theApp(hInstance);
    if (!theApp.Initialize())
      return 0;

    return theApp.Run();
  } catch (DxException &e) {
    MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
    return 0;
  }
}

ShapesApp::ShapesApp(HINSTANCE hInstance) : D3DApp(hInstance) {}

ShapesApp::~ShapesApp() {
  if (md3dDevice != nullptr)
    FlushCommandQueue();
}

bool ShapesApp::Initialize() {
  if (!D3DApp::Initialize())
    return false;

  // Reset the command list to prep for initialization commands.
  ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

  mCbvSrvDesciptorSize = md3dDevice->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  BuildRootSignature();
  BuildShadersAndInputLayout();
  BuildShapeGeometry();
  BuildMaterials();
  BuildRenderItems();
  BuildFrameResources();
  BuildPSOs();

  // Execute the initialization commands.
  ThrowIfFailed(mCommandList->Close());
  ID3D12CommandList *cmdsLists[] = {mCommandList.Get()};
  mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

  // Wait until initialization is complete.
  FlushCommandQueue();

  return true;
}

void ShapesApp::OnResize() {
  D3DApp::OnResize();

  // The window resized, so update the aspect ratio and recompute the projection
  // matrix.
  XMMATRIX P = XMMatrixPerspectiveFovLH(0.25f * MathHelper::Pi, AspectRatio(),
                                        1.0f, 1000.0f);
  XMStoreFloat4x4(&mProj, P);
}

void ShapesApp::Update(const GameTimer &gt) {
  OnKeyboardInput(gt);
  UpdateCamera(gt);

  // Cycle through the circular frame resource array.
  mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % gNumFrameResources;
  mCurrFrameResource = mFrameResources[mCurrFrameResourceIndex].get();

  // Has the GPU finished processing the commands of the current frame resource?
  // If not, wait until the GPU has completed commands up to this fence point.
  if (mCurrFrameResource->Fence != 0 &&
      mFence->GetCompletedValue() < mCurrFrameResource->Fence) {
    HANDLE eventHandle = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);
    ThrowIfFailed(
        mFence->SetEventOnCompletion(mCurrFrameResource->Fence, eventHandle));
    WaitForSingleObject(eventHandle, INFINITE);
    CloseHandle(eventHandle);
  }

  UpdateObjectCBs(gt);
  UpdateMaterialCBs(gt);
  UpdateMainPassCB(gt);
}

void ShapesApp::Draw(const GameTimer &gt) {
  auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;

  // Reuse the memory associated with command recording.
  // We can only reset when the associated command lists have finished execution
  // on the GPU.
  ThrowIfFailed(cmdListAlloc->Reset());

  // A command list can be reset after it has been added to the command queue
  // via ExecuteCommandList. Reusing the command list reuses memory.
  if (mIsWireframe) {
    ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(),
                                      mPSOs["opaque_wireframe"].Get()));
  } else {
    ThrowIfFailed(
        mCommandList->Reset(cmdListAlloc.Get(), mPSOs["opaque"].Get()));
  }

  mCommandList->RSSetViewports(1, &mScreenViewport);
  mCommandList->RSSetScissorRects(1, &mScissorRect);

  // Indicate a state transition on the resource usage.
  mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
                                       CurrentBackBuffer(),
                                       D3D12_RESOURCE_STATE_PRESENT,
                                       D3D12_RESOURCE_STATE_RENDER_TARGET));

  // Clear the back buffer and depth buffer.
  mCommandList->ClearRenderTargetView(CurrentBackBufferView(),
                                      Colors::LightSteelBlue, 0, nullptr);
  mCommandList->ClearDepthStencilView(
      DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
      1.0f, 0, 0, nullptr);

  // Specify the buffers we are going to render to.
  mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true,
                                   &DepthStencilView());

  mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

  auto passCB = mCurrFrameResource->PassCB->Resource();
  mCommandList->SetGraphicsRootConstantBufferView(
      2, passCB->GetGPUVirtualAddress());

  DrawRenderItems(mCommandList.Get(), mOpaqueRitems);

  // Indicate a state transition on the resource usage.
  mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
                                       CurrentBackBuffer(),
                                       D3D12_RESOURCE_STATE_RENDER_TARGET,
                                       D3D12_RESOURCE_STATE_PRESENT));

  // Done recording commands.
  ThrowIfFailed(mCommandList->Close());

  // Add the command list to the queue for execution.
  ID3D12CommandList *cmdsLists[] = {mCommandList.Get()};
  mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

  // Swap the back and front buffers
  ThrowIfFailed(mSwapChain->Present(0, 0));
  mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

  // Advance the fence value to mark commands up to this fence point.
  mCurrFrameResource->Fence = ++mCurrentFence;

  // Add an instruction to the command queue to set a new fence point.
  // Because we are on the GPU timeline, the new fence point won't be
  // set until the GPU finishes processing all the commands prior to this
  // Signal().
  mCommandQueue->Signal(mFence.Get(), mCurrentFence);
}

void ShapesApp::OnMouseDown(WPARAM btnState, int x, int y) {
  mLastMousePos.x = x;
  mLastMousePos.y = y;

  SetCapture(mhMainWnd);
}

void ShapesApp::OnMouseUp(WPARAM btnState, int x, int y) { ReleaseCapture(); }

void ShapesApp::OnMouseMove(WPARAM btnState, int x, int y) {
  if ((btnState & MK_LBUTTON) != 0) {
    // Make each pixel correspond to a quarter of a degree.
    float dx =
        XMConvertToRadians(0.25f * static_cast<float>(x - mLastMousePos.x));
    float dy =
        XMConvertToRadians(0.25f * static_cast<float>(y - mLastMousePos.y));

    // Update angles based on input to orbit camera around box.
    mTheta += dx;
    mPhi += dy;

    // Restrict the angle mPhi.
    mPhi = MathHelper::Clamp(mPhi, 0.1f, MathHelper::Pi - 0.1f);
  } else if ((btnState & MK_RBUTTON) != 0) {
    // Make each pixel correspond to 0.2 unit in the scene.
    float dx = 0.05f * static_cast<float>(x - mLastMousePos.x);
    float dy = 0.05f * static_cast<float>(y - mLastMousePos.y);

    // Update the camera radius based on input.
    mRadius += dx - dy;

    // Restrict the radius.
    mRadius = MathHelper::Clamp(mRadius, 5.0f, 150.0f);
  }

  mLastMousePos.x = x;
  mLastMousePos.y = y;
}

void ShapesApp::OnKeyboardInput(const GameTimer &gt) {
  if (GetAsyncKeyState('1') & 0x8000)
    mIsWireframe = true;
  else
    mIsWireframe = false;
}

void ShapesApp::UpdateCamera(const GameTimer &gt) {
  // Convert Spherical to Cartesian coordinates.
  mEyePos.x = mRadius * sinf(mPhi) * cosf(mTheta);
  mEyePos.z = mRadius * sinf(mPhi) * sinf(mTheta);
  mEyePos.y = mRadius * cosf(mPhi);

  // Build the view matrix.
  XMVECTOR pos = XMVectorSet(mEyePos.x, mEyePos.y, mEyePos.z, 1.0f);
  XMVECTOR target = XMVectorZero();
  XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

  XMMATRIX view = XMMatrixLookAtLH(pos, target, up);
  XMStoreFloat4x4(&mView, view);
}

void ShapesApp::UpdateObjectCBs(const GameTimer &gt) {
  auto currObjectCB = mCurrFrameResource->ObjectCB.get();
  auto vec = &mCurrFrameResource->ObjectConstantVec;
  for (auto &e : mAllRitems) {
    // Only update the cbuffer data if the constants have changed.
    // This needs to be tracked per frame resource.
    if (e->NumFramesDirty > 0) {
      XMMATRIX world = XMLoadFloat4x4(&e->World);

      ObjectConstants objConstants;
      XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(world));

      currObjectCB->CopyData(e->ObjCBIndex, objConstants);

      (*vec)[e->ObjCBIndex] = objConstants;
      // Next FrameResource need to be updated too.
      e->NumFramesDirty--;
    }
  }
}

void ShapesApp::UpdateMainPassCB(const GameTimer &gt) {
  XMMATRIX view = XMLoadFloat4x4(&mView);
  XMMATRIX proj = XMLoadFloat4x4(&mProj);

  XMMATRIX viewProj = XMMatrixMultiply(view, proj);
  XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
  XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
  XMMATRIX invViewProj =
      XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);

  XMStoreFloat4x4(&mMainPassCB.View, XMMatrixTranspose(view));
  XMStoreFloat4x4(&mMainPassCB.InvView, XMMatrixTranspose(invView));
  XMStoreFloat4x4(&mMainPassCB.Proj, XMMatrixTranspose(proj));
  XMStoreFloat4x4(&mMainPassCB.InvProj, XMMatrixTranspose(invProj));
  XMStoreFloat4x4(&mMainPassCB.ViewProj, XMMatrixTranspose(viewProj));
  XMStoreFloat4x4(&mMainPassCB.InvViewProj, XMMatrixTranspose(invViewProj));
  mMainPassCB.EyePosW = mEyePos;
  mMainPassCB.RenderTargetSize =
      XMFLOAT2((float)mClientWidth, (float)mClientHeight);
  mMainPassCB.InvRenderTargetSize =
      XMFLOAT2(1.0f / mClientWidth, 1.0f / mClientHeight);
  mMainPassCB.NearZ = 1.0f;
  mMainPassCB.FarZ = 1000.0f;
  mMainPassCB.TotalTime = gt.TotalTime();
  mMainPassCB.DeltaTime = gt.DeltaTime();

  mMainPassCB.AmbientLight = {0.25f, 0.25f, 0.35f, 1.0f};
  // XMVECTOR lightDir1 =
  //    -MathHelper::SphericalToCartesian(1.0f, XM_PI, XM_PIDIV4);
  // XMStoreFloat3(&mMainPassCB.Lights[0].Direction, lightDir1);
  // XMVECTOR lightDir2 = -MathHelper::SphericalToCartesian(1.0f, 0, XM_PI);
  // XMStoreFloat3(&mMainPassCB.Lights[1].Direction, lightDir2);
  // XMVECTOR lightDir3 = -MathHelper::SphericalToCartesian(1.0f, 0, -XM_PI);
  // XMStoreFloat3(&mMainPassCB.Lights[2].Direction, lightDir3);

  // mMainPassCB.Lights[0].Strength = {1.0f, 1.0f, 1.0f};
  // mMainPassCB.Lights[1].Strength = {1.0f, 1.0f, 1.0f};
  // mMainPassCB.Lights[2].Strength = {1.0f, 1.0f, 1.0f};

  auto currPassCB = mCurrFrameResource->PassCB.get();
  currPassCB->CopyData(0, mMainPassCB);
}

void ShapesApp::UpdateMaterialCBs(const GameTimer &gt) {
  auto currMaterialCB = mCurrFrameResource->MaterialCB.get();
  for (auto &e : mMaterials) {
    Material *mat = e.second.get();
    if (mat->NumFramesDirty > 0) {
      XMMATRIX matTransform = XMLoadFloat4x4(&mat->MatTransform);

      MaterialConstants matContants;
      matContants.DiffuseAlbedo = mat->DiffuseAlbedo;
      matContants.FresnelR0 = mat->FresnelR0;
      matContants.Roughness = mat->Roughness;

      currMaterialCB->CopyData(mat->MatCBIndex, matContants);
      mat->NumFramesDirty--;
    }
  }
}

void ShapesApp::BuildDescriptorHeaps() {
  UINT objCount = (UINT)mOpaqueRitems.size();

  // Need a CBV descriptor for each object for each frame resource,
  // +1 for the perPass CBV for each frame resource.
  UINT numDescriptors = (objCount + 1) * gNumFrameResources;

  // Save an offset to the start of the pass CBVs.  These are the last 3
  // descriptors.
  mPassCbvOffset = objCount * gNumFrameResources;

  D3D12_DESCRIPTOR_HEAP_DESC cbvHeapDesc;
  cbvHeapDesc.NumDescriptors = numDescriptors;
  cbvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  cbvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  cbvHeapDesc.NodeMask = 0;
}

void ShapesApp::BuildConstantBufferViews() {
  UINT objCBByteSize =
      d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));

  UINT objCount = (UINT)mOpaqueRitems.size();

  // Need a CBV descriptor for each object for each frame resource.
  for (int frameIndex = 0; frameIndex < gNumFrameResources; ++frameIndex) {
    auto objectCB = mFrameResources[frameIndex]->ObjectCB->Resource();
    for (UINT i = 0; i < objCount; ++i) {
      D3D12_GPU_VIRTUAL_ADDRESS cbAddress = objectCB->GetGPUVirtualAddress();

      // Offset to the ith object constant buffer in the buffer.
      cbAddress += i * objCBByteSize;

      // Offset to the object cbv in the descriptor heap.
      D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc;
      cbvDesc.BufferLocation = cbAddress;
      cbvDesc.SizeInBytes = objCBByteSize;
    }
  }

  UINT passCBByteSize =
      d3dUtil::CalcConstantBufferByteSize(sizeof(PassConstants));

  // Last three descriptors are the pass CBVs for each frame resource.
  for (int frameIndex = 0; frameIndex < gNumFrameResources; ++frameIndex) {
    auto passCB = mFrameResources[frameIndex]->PassCB->Resource();
    D3D12_GPU_VIRTUAL_ADDRESS cbAddress = passCB->GetGPUVirtualAddress();

    // Offset to the pass cbv in the descriptor heap.
  }
}

void ShapesApp::BuildRootSignature() {
  // Root parameter can be a table, root descriptor or root constants.
  CD3DX12_ROOT_PARAMETER slotRootParameter[3];

  // Create root CBVs.
  slotRootParameter[0].InitAsConstantBufferView(0);
  slotRootParameter[1].InitAsConstantBufferView(1);
  slotRootParameter[2].InitAsConstantBufferView(2);

  // A root signature is an array of root parameters.
  CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
      3, slotRootParameter, 0, nullptr,
      D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

  // create a root signature with a single slot which points to a descriptor
  // range consisting of a single constant buffer
  ComPtr<ID3DBlob> serializedRootSig = nullptr;
  ComPtr<ID3DBlob> errorBlob = nullptr;
  HRESULT hr = D3D12SerializeRootSignature(
      &rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
      serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

  if (errorBlob != nullptr) {
    ::OutputDebugStringA((char *)errorBlob->GetBufferPointer());
  }
  ThrowIfFailed(hr);

  ThrowIfFailed(md3dDevice->CreateRootSignature(
      0, serializedRootSig->GetBufferPointer(),
      serializedRootSig->GetBufferSize(),
      IID_PPV_ARGS(mRootSignature.GetAddressOf())));
}

void ShapesApp::BuildShadersAndInputLayout() {
  mShaders["standardVS"] =
      d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr, "VS", "vs_5_0");
  mShaders["opaquePS"] =
      d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr, "PS", "ps_5_0");

  mInputLayout = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"NORMAL", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  };
}

void ShapesApp::BuildShapeGeometry() {
  GeometryGenerator geoGen;
  GeometryGenerator::MeshData box = geoGen.CreateBox(1.5f, 0.5f, 1.5f, 3);
  GeometryGenerator::MeshData grid = geoGen.CreateGrid(20.0f, 30.0f, 60, 40);
  GeometryGenerator::MeshData sphere = geoGen.CreateGeosphere(0.5f, 3);
  GeometryGenerator::MeshData cylinder =
      geoGen.CreateCylinder(0.5f, 0.3f, 3.0f, 20, 20);

  //
  // We are concatenating all the geometry into one big vertex/index buffer.  So
  // define the regions in the buffer each submesh covers.
  //

  // Cache the vertex offsets to each object in the concatenated vertex buffer.
  UINT boxVertexOffset = 0;
  UINT gridVertexOffset = (UINT)box.Vertices.size();
  UINT sphereVertexOffset = gridVertexOffset + (UINT)grid.Vertices.size();
  UINT cylinderVertexOffset = sphereVertexOffset + (UINT)sphere.Vertices.size();

  // Cache the starting index for each object in the concatenated index buffer.
  UINT boxIndexOffset = 0;
  UINT gridIndexOffset = (UINT)box.Indices32.size();
  UINT sphereIndexOffset = gridIndexOffset + (UINT)grid.Indices32.size();
  UINT cylinderIndexOffset = sphereIndexOffset + (UINT)sphere.Indices32.size();

  // Define the SubmeshGeometry that cover different
  // regions of the vertex/index buffers.

  SubmeshGeometry boxSubmesh;
  boxSubmesh.IndexCount = (UINT)box.Indices32.size();
  boxSubmesh.StartIndexLocation = boxIndexOffset;
  boxSubmesh.BaseVertexLocation = boxVertexOffset;

  SubmeshGeometry gridSubmesh;
  gridSubmesh.IndexCount = (UINT)grid.Indices32.size();
  gridSubmesh.StartIndexLocation = gridIndexOffset;
  gridSubmesh.BaseVertexLocation = gridVertexOffset;

  SubmeshGeometry sphereSubmesh;
  sphereSubmesh.IndexCount = (UINT)sphere.Indices32.size();
  sphereSubmesh.StartIndexLocation = sphereIndexOffset;
  sphereSubmesh.BaseVertexLocation = sphereVertexOffset;

  SubmeshGeometry cylinderSubmesh;
  cylinderSubmesh.IndexCount = (UINT)cylinder.Indices32.size();
  cylinderSubmesh.StartIndexLocation = cylinderIndexOffset;
  cylinderSubmesh.BaseVertexLocation = cylinderVertexOffset;

  //
  // Extract the vertex elements we are interested in and pack the
  // vertices of all the meshes into one vertex buffer.
  //

  auto totalVertexCount = box.Vertices.size() + grid.Vertices.size() +
                          sphere.Vertices.size() + cylinder.Vertices.size();

  std::vector<Vertex> vertices(totalVertexCount);

  UINT k = 0;
  for (size_t i = 0; i < box.Vertices.size(); ++i, ++k) {
    vertices[k].Pos = box.Vertices[i].Position;
    vertices[k].Normal = box.Vertices[i].Normal;
  }

  for (size_t i = 0; i < grid.Vertices.size(); ++i, ++k) {
    vertices[k].Pos = grid.Vertices[i].Position;
    vertices[k].Normal = grid.Vertices[i].Normal;
  }

  for (size_t i = 0; i < sphere.Vertices.size(); ++i, ++k) {
    vertices[k].Pos = sphere.Vertices[i].Position;
    vertices[k].Normal = sphere.Vertices[i].Normal;
  }

  for (size_t i = 0; i < cylinder.Vertices.size(); ++i, ++k) {
    vertices[k].Pos = cylinder.Vertices[i].Position;
    vertices[k].Normal = cylinder.Vertices[i].Normal;
  }

  std::vector<std::uint16_t> indices;
  indices.insert(indices.end(), std::begin(box.GetIndices16()),
                 std::end(box.GetIndices16()));
  indices.insert(indices.end(), std::begin(grid.GetIndices16()),
                 std::end(grid.GetIndices16()));
  indices.insert(indices.end(), std::begin(sphere.GetIndices16()),
                 std::end(sphere.GetIndices16()));
  indices.insert(indices.end(), std::begin(cylinder.GetIndices16()),
                 std::end(cylinder.GetIndices16()));

  const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);
  const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint16_t);

  auto geo = std::make_unique<MeshGeometry>();
  geo->Name = "shapeGeo";

  ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
  CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(),
             vbByteSize);

  ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
  CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(),
             ibByteSize);

  geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(
      md3dDevice.Get(), mCommandList.Get(), vertices.data(), vbByteSize,
      geo->VertexBufferUploader);

  geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(
      md3dDevice.Get(), mCommandList.Get(), indices.data(), ibByteSize,
      geo->IndexBufferUploader);

  geo->VertexByteStride = sizeof(Vertex);
  geo->VertexBufferByteSize = vbByteSize;
  geo->IndexFormat = DXGI_FORMAT_R16_UINT;
  geo->IndexBufferByteSize = ibByteSize;

  geo->DrawArgs["box"] = boxSubmesh;
  geo->DrawArgs["grid"] = gridSubmesh;
  geo->DrawArgs["sphere"] = sphereSubmesh;
  geo->DrawArgs["cylinder"] = cylinderSubmesh;

  mGeometries[geo->Name] = std::move(geo);
}

void ShapesApp::BuildPSOs() {
  D3D12_GRAPHICS_PIPELINE_STATE_DESC opaquePsoDesc;

  //
  // PSO for opaque objects.
  //
  ZeroMemory(&opaquePsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
  opaquePsoDesc.InputLayout = {mInputLayout.data(), (UINT)mInputLayout.size()};
  opaquePsoDesc.pRootSignature = mRootSignature.Get();
  opaquePsoDesc.VS = {
      reinterpret_cast<BYTE *>(mShaders["standardVS"]->GetBufferPointer()),
      mShaders["standardVS"]->GetBufferSize()};
  opaquePsoDesc.PS = {
      reinterpret_cast<BYTE *>(mShaders["opaquePS"]->GetBufferPointer()),
      mShaders["opaquePS"]->GetBufferSize()};
  opaquePsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
  opaquePsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  opaquePsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
  opaquePsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
  opaquePsoDesc.SampleMask = UINT_MAX;
  opaquePsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  opaquePsoDesc.NumRenderTargets = 1;
  opaquePsoDesc.RTVFormats[0] = mBackBufferFormat;
  opaquePsoDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
  opaquePsoDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
  opaquePsoDesc.DSVFormat = mDepthStencilFormat;
  ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(
      &opaquePsoDesc, IID_PPV_ARGS(&mPSOs["opaque"])));

  //
  // PSO for opaque wireframe objects.
  //

  D3D12_GRAPHICS_PIPELINE_STATE_DESC opaqueWireframePsoDesc = opaquePsoDesc;
  opaqueWireframePsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
  ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(
      &opaqueWireframePsoDesc, IID_PPV_ARGS(&mPSOs["opaque_wireframe"])));
}

void ShapesApp::BuildFrameResources() {
  for (int i = 0; i < gNumFrameResources; ++i) {
    mFrameResources.push_back(std::make_unique<FrameResource>(
        md3dDevice.Get(), 1, (UINT)mAllRitems.size(), (UINT)mMaterials.size()));
  }
}

void ShapesApp ::BuildMaterials() {
  auto box = std::make_unique<Material>();
  box->Name = "box";
  box->MatCBIndex = 0;
  box->DiffuseAlbedo = XMFLOAT4(0.2f, 0.6f, 0.2f, 1.0f);
  box->FresnelR0 = XMFLOAT3(0.01f, 0.01f, 0.01f);
  box->Roughness = 0.125f;
  mMaterials["box"] = std::move(box);

  auto grid = std::make_unique<Material>();
  grid->Name = "grid";
  grid->MatCBIndex = 1;
  grid->DiffuseAlbedo = XMFLOAT4(0.5f, 0.1f, 0.8f, 1.0f);
  grid->FresnelR0 = XMFLOAT3(0.1f, 0.1f, 0.1f);
  grid->Roughness = 0.0f;
  mMaterials["grid"] = std::move(grid);

  auto cylinder = std::make_unique<Material>();
  cylinder->Name = "cylinder";
  cylinder->MatCBIndex = 2;
  cylinder->DiffuseAlbedo = XMFLOAT4(0.1f, 0.1f, 0.1f, 1.0f);
  cylinder->FresnelR0 = XMFLOAT3(0.1f, 0.5f, 0.1f);
  cylinder->Roughness = 0.3f;
  mMaterials["cylinder"] = std::move(cylinder);

  auto sphere = std::make_unique<Material>();
  sphere->Name = "sphere";
  sphere->MatCBIndex = 3;
  sphere->DiffuseAlbedo = XMFLOAT4(0.9f, 0.9f, 0.9f, 1.0f);
  sphere->FresnelR0 = XMFLOAT3(0.5f, 0.5f, 0.5f);
  sphere->Roughness = 0.8f;
  mMaterials["sphere"] = std::move(sphere);
}

void ShapesApp::BuildRenderItems() {
  auto boxRitem = std::make_unique<RenderItem>();
  XMStoreFloat4x4(&boxRitem->World, XMMatrixScaling(2.0f, 2.0f, 2.0f) *
                                        XMMatrixTranslation(0.0f, 0.5f, 0.0f));
  boxRitem->ObjCBIndex = 0;
  boxRitem->Mat = mMaterials["box"].get();
  boxRitem->Geo = mGeometries["shapeGeo"].get();
  boxRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  boxRitem->IndexCount = boxRitem->Geo->DrawArgs["box"].IndexCount;
  boxRitem->StartIndexLocation =
      boxRitem->Geo->DrawArgs["box"].StartIndexLocation;
  boxRitem->BaseVertexLocation =
      boxRitem->Geo->DrawArgs["box"].BaseVertexLocation;
  mAllRitems.push_back(std::move(boxRitem));

  auto gridRitem = std::make_unique<RenderItem>();
  gridRitem->World = MathHelper::Identity4x4();
  gridRitem->ObjCBIndex = 1;
  gridRitem->Mat = mMaterials["grid"].get();
  gridRitem->Geo = mGeometries["shapeGeo"].get();
  gridRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  gridRitem->IndexCount = gridRitem->Geo->DrawArgs["grid"].IndexCount;
  gridRitem->StartIndexLocation =
      gridRitem->Geo->DrawArgs["grid"].StartIndexLocation;
  gridRitem->BaseVertexLocation =
      gridRitem->Geo->DrawArgs["grid"].BaseVertexLocation;
  mAllRitems.push_back(std::move(gridRitem));

  UINT objCBIndex = 2;
  UINT lightIndex = 0;
  for (int i = 0; i < 5; ++i) {
    auto leftCylRitem = std::make_unique<RenderItem>();
    auto rightCylRitem = std::make_unique<RenderItem>();
    auto leftSphereRitem = std::make_unique<RenderItem>();
    auto rightSphereRitem = std::make_unique<RenderItem>();

    XMMATRIX leftCylWorld = XMMatrixTranslation(-5.0f, 1.5f, -10.0f + i * 5.0f);
    XMMATRIX rightCylWorld =
        XMMatrixTranslation(+5.0f, 1.5f, -10.0f + i * 5.0f);

    XMMATRIX leftSphereWorld =
        XMMatrixTranslation(-5.0f, 3.5f, -10.0f + i * 5.0f);
    XMMATRIX rightSphereWorld =
        XMMatrixTranslation(+5.0f, 3.5f, -10.0f + i * 5.0f);

    XMStoreFloat4x4(&leftCylRitem->World, leftCylWorld);
    leftCylRitem->ObjCBIndex = objCBIndex++;
    leftCylRitem->Mat = mMaterials["cylinder"].get();
    leftCylRitem->Geo = mGeometries["shapeGeo"].get();
    leftCylRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    leftCylRitem->IndexCount =
        leftCylRitem->Geo->DrawArgs["cylinder"].IndexCount;
    leftCylRitem->StartIndexLocation =
        leftCylRitem->Geo->DrawArgs["cylinder"].StartIndexLocation;
    leftCylRitem->BaseVertexLocation =
        leftCylRitem->Geo->DrawArgs["cylinder"].BaseVertexLocation;

    XMStoreFloat4x4(&rightCylRitem->World, rightCylWorld);
    rightCylRitem->ObjCBIndex = objCBIndex++;
    rightCylRitem->Mat = mMaterials["cylinder"].get();
    rightCylRitem->Geo = mGeometries["shapeGeo"].get();
    rightCylRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    rightCylRitem->IndexCount =
        rightCylRitem->Geo->DrawArgs["cylinder"].IndexCount;
    rightCylRitem->StartIndexLocation =
        rightCylRitem->Geo->DrawArgs["cylinder"].StartIndexLocation;
    rightCylRitem->BaseVertexLocation =
        rightCylRitem->Geo->DrawArgs["cylinder"].BaseVertexLocation;

    XMStoreFloat4x4(&leftSphereRitem->World, leftSphereWorld);
    leftSphereRitem->ObjCBIndex = objCBIndex++;
    leftSphereRitem->Mat = mMaterials["sphere"].get();
    leftSphereRitem->Geo = mGeometries["shapeGeo"].get();
    leftSphereRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    leftSphereRitem->IndexCount =
        leftSphereRitem->Geo->DrawArgs["sphere"].IndexCount;
    leftSphereRitem->StartIndexLocation =
        leftSphereRitem->Geo->DrawArgs["sphere"].StartIndexLocation;
    leftSphereRitem->BaseVertexLocation =
        leftSphereRitem->Geo->DrawArgs["sphere"].BaseVertexLocation;

    Light pointLight;
    pointLight.FalloffStart = 0;
    pointLight.FalloffEnd = 50.0f;
    pointLight.Strength = XMFLOAT3(1.0f, 1.0f, 1.0f);
    pointLight.SpotPower = 0.3;
    XMStoreFloat3(&pointLight.Position, leftSphereWorld.r[3]);
    XMStoreFloat3(&pointLight.Direction,
                  XMVector3Normalize(-XMVectorSet(pointLight.Position.x,
                                                 pointLight.Position.y,
                                                 pointLight.Position.z, 1.0f)));
    mMainPassCB.Lights[lightIndex++] = pointLight;

    XMStoreFloat4x4(&rightSphereRitem->World, rightSphereWorld);
    rightSphereRitem->ObjCBIndex = objCBIndex++;
    rightSphereRitem->Mat = mMaterials["sphere"].get();
    rightSphereRitem->Geo = mGeometries["shapeGeo"].get();
    rightSphereRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    rightSphereRitem->IndexCount =
        rightSphereRitem->Geo->DrawArgs["sphere"].IndexCount;
    rightSphereRitem->StartIndexLocation =
        rightSphereRitem->Geo->DrawArgs["sphere"].StartIndexLocation;
    rightSphereRitem->BaseVertexLocation =
        rightSphereRitem->Geo->DrawArgs["sphere"].BaseVertexLocation;

    XMStoreFloat3(&pointLight.Position, rightSphereWorld.r[3]);
    XMStoreFloat3(&pointLight.Direction,
                  XMVector3Normalize(-XMVectorSet(pointLight.Position.x,
                                                 pointLight.Position.y,
                                                 pointLight.Position.z, 1.0f)));
    mMainPassCB.Lights[lightIndex++] = pointLight;

    mAllRitems.push_back(std::move(leftCylRitem));
    mAllRitems.push_back(std::move(rightCylRitem));
    mAllRitems.push_back(std::move(leftSphereRitem));
    mAllRitems.push_back(std::move(rightSphereRitem));
  }

  // All the render items are opaque.
  for (auto &e : mAllRitems)
    mOpaqueRitems.push_back(e.get());
}

void ShapesApp::DrawRenderItems(ID3D12GraphicsCommandList *cmdList,
                                const std::vector<RenderItem *> &ritems) {
  UINT objCBByteSize =
      d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
  UINT matCBByteSize =
      d3dUtil::CalcConstantBufferByteSize(sizeof(MaterialConstants));

  auto objectCB = mCurrFrameResource->ObjectCB->Resource();
  auto matCB = mCurrFrameResource->MaterialCB->Resource();
  // For each render item...
  for (size_t i = 0; i < ritems.size(); ++i) {
    auto ri = ritems[i];

    cmdList->IASetVertexBuffers(0, 1, &ri->Geo->VertexBufferView());
    cmdList->IASetIndexBuffer(&ri->Geo->IndexBufferView());
    cmdList->IASetPrimitiveTopology(ri->PrimitiveType);

    auto objCBAddress =
        objectCB->GetGPUVirtualAddress() + ri->ObjCBIndex * objCBByteSize;
    auto matCBAddress =
        matCB->GetGPUVirtualAddress() + ri->Mat->MatCBIndex * matCBByteSize;
    cmdList->SetGraphicsRootConstantBufferView(0, objCBAddress);
    cmdList->SetGraphicsRootConstantBufferView(1, matCBAddress);

    cmdList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation,
                                  ri->BaseVertexLocation, 0);
  }
}
