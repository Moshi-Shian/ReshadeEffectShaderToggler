#include "SceneInputSync.h"
#include "SceneInputPoolPolicy.h"
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
using namespace Rendering;
using Microsoft::WRL::ComPtr;
static void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static void Hr(HRESULT result) { Check(SUCCEEDED(result), "Native lifecycle operation failed"); }

int main() {
    try {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
        ComPtr<IDXGIFactory4> factory;
        Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter> warp;
        Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        ComPtr<ID3D12Device> device;
        Hr(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        D3D12_COMMAND_QUEUE_DESC desc{};
        ComPtr<ID3D12CommandQueue> queue;
        Hr(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)));
        auto done = std::make_shared<SceneFence>();
        Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done->fence)));
        auto idle = [&] {
            Hr(queue->Signal(done->fence.Get(), ++done->value));
            Hr(done->fence->SetEventOnCompletion(done->value, nullptr));
        };
        auto buffer = [&](D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = type;
            heap.CreationNodeMask = heap.VisibleNodeMask = 1;
            D3D12_RESOURCE_DESC resource{};
            resource.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            resource.Width = 256;
            resource.Height = resource.DepthOrArraySize = resource.MipLevels = 1;
            resource.SampleDesc.Count = 1;
            resource.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> result;
            Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &resource, state, nullptr, IID_PPV_ARGS(&result)));
            return result;
        };
        struct Image { SceneInputOwnership ownership; ComPtr<ID3D12Resource> resource; };
        struct Recording {
            ComPtr<ID3D12CommandAllocator> allocator;
            ComPtr<ID3D12GraphicsCommandList> commands;
            ComPtr<ID3D12Resource> input, output;
        };
        std::array<Image, SceneSplitImageLimit> images;
        std::array<Recording, SceneSplitImageLimit + 2> recordings;
        auto record = [&](size_t index, size_t imageIndex) {
            auto& r = recordings[index];
            auto& image = images[imageIndex];
            Check(image.ownership.CanAcquire(&r), "Recording acquired an image still owned elsewhere");
            image.ownership.Acquire(&r, &r);
            if (!image.resource) image.resource = buffer(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
            r.input = buffer(D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            r.output = buffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
            void* mapped = nullptr;
            D3D12_RANGE unread{};
            Hr(r.input->Map(0, &unread, &mapped));
            *static_cast<UINT64*>(mapped) = 100 + index;
            r.input->Unmap(0, nullptr);
            Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&r.allocator)));
            Hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, r.allocator.Get(), nullptr, IID_PPV_ARGS(&r.commands)));
            r.commands->CopyBufferRegion(image.resource.Get(), 0, r.input.Get(), 0, sizeof(UINT64));
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition = {image.resource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                  D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE};
            r.commands->ResourceBarrier(1, &barrier);
            r.commands->CopyBufferRegion(r.output.Get(), 0, image.resource.Get(), 0, sizeof(UINT64));
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            r.commands->ResourceBarrier(1, &barrier);
            Hr(r.commands->Close());
        };
        auto verify = [&](size_t index) {
            void* mapped = nullptr;
            auto& r = recordings[index];
            Hr(r.output->Map(0, nullptr, &mapped));
            Check(*static_cast<const UINT64*>(mapped) == 100 + index, "Lifecycle output mismatch");
            D3D12_RANGE unmodified{};
            r.output->Unmap(0, &unmodified);
        };
        std::array<ID3D12CommandList*, SceneSplitImageLimit> batch;
        for (size_t i = 0; i < images.size(); ++i) {
            record(i, i);
            batch[i] = recordings[i].commands.Get();
        }
        queue->ExecuteCommandLists(static_cast<UINT>(batch.size()), batch.data());
        idle(); // The GPU is completely idle; none of these recordings was reset.
        for (size_t i = 0; i < images.size(); ++i) {
            verify(i);
            Check(!images[i].ownership.CanAcquire(&recordings[8]), "Completion-only release escaped the ownership policy");
        }
        const auto* rejection = SceneAllocationRejection(true, 8, 1275 * SceneMiB, 159 * SceneMiB,
            1536 * SceneMiB, {15339 * SceneMiB, 5054 * SceneMiB, true});
        Check(rejection && std::string(rejection) == "Scene input: active image capacity reached", "Reporter exhaustion not reproduced");
        std::puts("REPRODUCED: GPU idle, eight valid closed recordings, ninth request rejected despite ample memory headroom.");

        // Legal resubmission proves that completion was not the end of the
        // recording's lifetime. Its original scratch and resources stay alive.
        queue->ExecuteCommandLists(static_cast<UINT>(batch.size()), batch.data());
        idle();
        for (size_t i = 0; i < images.size(); ++i) verify(i);

        // Match ReShade's event order: retire the old recording BEFORE Reset.
        images[0].ownership.Retire(&recordings[0], true, {{done, done->value}});
        Hr(recordings[0].commands->Reset(recordings[0].allocator.Get(), nullptr));
        record(8, 0);
        ID3D12CommandList* replacement[]{recordings[8].commands.Get()};
        Hr(WaitSceneInputTickets(queue.Get(), images[0].ownership.dependencies));
        queue->ExecuteCommandLists(1, replacement);
        idle();
        verify(8);
        Check(!images[0].ownership.CanAcquire(&recordings[0]), "Reset old owner took back replacement image");
        images[0].ownership.Retire(&recordings[0], true, {}); // Duplicate/old retirement.
        Check(images[0].ownership.owner == &recordings[8], "Old retirement released replacement recording");
        std::puts("Reset released exactly one image; replacement output and other live recordings remained valid.");

        // ReShade invokes every per-list callback before the batch's native
        // ExecuteCommandLists. Signals made there precede, not follow, work.
        ComPtr<ID3D12Fence> markers, gate;
        Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&markers)));
        Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
        Hr(queue->Signal(markers.Get(), 1)); // Callback for first list.
        Hr(queue->Signal(markers.Get(), 2)); // Callback for second list.
        Hr(queue->Wait(gate.Get(), 1)); // Hold the batch so event order is measurable.
        ID3D12CommandList* delayed[]{recordings[1].commands.Get(), recordings[2].commands.Get()};
        queue->ExecuteCommandLists(2, delayed);
        const auto delayedDone = ++done->value;
        Hr(queue->Signal(done->fence.Get(), delayedDone));
        Hr(markers->SetEventOnCompletion(2, nullptr));
        Check(done->fence->GetCompletedValue() < delayedDone, "Pre-submit marker incorrectly followed the batch");
        // Reset the command-list object while its old native commands are in
        // flight. A fresh allocator is required for the next recording.
        ComPtr<ID3D12CommandAllocator> fresh;
        Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&fresh)));
        images[1].ownership.Retire(&recordings[1], true, {{done, delayedDone}});
        Hr(recordings[1].commands->Reset(fresh.Get(), nullptr));
        Check(images[1].ownership.CanAcquire(&recordings[9]), "Reset did not permit fenced image handoff");
        Check(!SceneTicketsComplete(images[1].ownership.dependencies), "Pending GPU dependency was dropped at reset");
        Hr(gate->Signal(1));
        Hr(done->fence->SetEventOnCompletion(delayedDone, nullptr));
        Check(SceneTicketsComplete(images[1].ownership.dependencies), "Reset completion dependency did not finish");
        verify(1); verify(2);
        std::puts("Batched pre-submit markers completed before gated GPU work; reset preserved its pending completion dependency.");

        images[1].ownership.Acquire(&recordings[9], &recordings[9]);
        const auto inherited = images[1].ownership.dependencies;
        images[1].ownership.Retire(&recordings[9], true, {}); // Never submitted.
        Check(images[1].ownership.dependencies.size() == inherited.size(), "Unsubmitted reset lost inherited tickets");
        Check(images[1].ownership.CanAcquire(&recordings[0]), "Unsubmitted recording leaked ownership");
        images[1].ownership.Retire(&recordings[9], true, {}); // Idempotent.
        std::puts("Unsubmitted and duplicate retirement preserved dependencies without leaking ownership.");

        ComPtr<ID3D12InfoQueue> messages;
        if (SUCCEEDED(device.As(&messages))) {
            for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
                SIZE_T size = 0;
                Hr(messages->GetMessage(i, nullptr, &size));
                std::vector<char> bytes(size);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
                Hr(messages->GetMessage(i, message, &size));
                Check(message->Severity > D3D12_MESSAGE_SEVERITY_ERROR, message->pDescription);
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what()); return 1;
    }
}
