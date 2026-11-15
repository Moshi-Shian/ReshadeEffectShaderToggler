# REST Enhanced Scene input preview 6

**Follow-up:** The reporter confirmed much less flicker but still exhausted eight active pairs at 4K. The [local lifetime investigation](scene-input-lifetime-investigation.md) reproduces this with the GPU idle. Preview 6 remains experimental; another pool expansion is not the planned remedy.

Preview 5's test results confirmed correct scene tint and technique colours, but flicker remained in No TAA, TAA, DLSS and FSR. During the DLSS failure, all four private image pairs were held by active command recordings. GPU handoffs were functioning; waiting for retired recordings alone could not provide another image.

## Changes and memory cost

- The opt-in Warframe compositor split can grow on demand to **eight image pairs**, subject to memory checks. At 3818×1600, eight full-precision pairs require about **933 MiB**, compared with about 466 MiB for four. Images are allocated only as needed.
- The image pool is bounded by **1.5 GiB** and **one quarter of Windows' current local GPU memory budget**. Each additional allocation must fit within half the reported remaining budget headroom. If the budget query is unavailable, the previous 512 MiB limit applies. Ordinary scene input retains its 256 MiB maximum.
- Memory accounting uses D3D12's allocation sizes, including alignment, rather than texel payload alone. The budget is queried at most four times per second during allocation pressure, with intervening private allocations accounted for locally.
- Sequential draws prefer an image already owned by their recording. Active recordings keep independent images; immutable descriptors and GPU-ordered retirement follow Preview 5.
- Failed image allocations now distinguish the image-count limit, byte limit and insufficient GPU budget headroom. Copied diagnostics preserve the pool classification **at the failed attempt**, including compatible active, available, incompatible and unsafe images, requested size and memory budget.

The previous TAA report showed three active pairs and one retired descriptor set. Its status and current pool counters were copied at different times; they cannot establish why that earlier attempt failed. The new failure snapshot removes this ambiguity. An image belonging to another size/format or one marked unsafe is not made reusable merely by retirement.

Windows' budget is a process-wide target, not the card's advertised VRAM capacity. These allocation-admission checks limit growth but cannot reserve memory against allocations made concurrently by the game or other software. Existing in-flight images are not destroyed if Windows later reduces the budget. REST skips processing when a bound or native allocation fails. It does not lower HDR precision to fit more images. See [Microsoft's memory budget documentation](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/ns-dxgi1_4-dxgi_query_video_memory_info).

## Install and test

1. Replace the add-on with the matching architecture from **REST-Scene-Input-Preview-6-Modern.zip**, then restart the game. Warframe uses `.addon64`. Keep the existing pinned DXC pair in `REST-DXC` and the compositor-split setting enabled. The [Preview 4 guide](scene-input-preview-4.md) covers these files and settings.
2. Keep native HDR, RenoDX off, **Pixel / Slot 2 / Binding 0**, Auto Scene Colour off, clamping off and flip off. Use the existing selection for each anti-aliasing mode.
3. Test **REST_SceneBoundaryCheck** alone with motion blur on. End shader hunting and close the overlays. Move around and open/close the ESC menu and the UI elements that triggered flicker. Repeat No TAA, TAA, DLSS and FSR where available.
4. If the tint stays stable, try the desired Marty’s Mods technique alone and a longer session. Note stability and any performance change.

If it still flickers, paste **Copy preview diagnostics** from this version, including **Last pool skip**. A new frame capture is not needed for the current allocation investigation. If there are no pool skips, the diagnostic processing status will help separate any remaining rendering problem from resource pressure.

## Verification and status

The native D3D12 test keeps eight command recordings active simultaneously, executes them in reverse order, then resubmits them after completion without resetting the recordings. Every output must remain exact and independent. The existing 96-submission delayed-GPU test across two queues still runs.

Production memory-policy tests reproduce the four-pair exhaustion at the reporter's resolution, admit eight recordings with sufficient memory, reject a ninth, and cover retirement reuse, unknown/low/exhausted budgets, hard caps, overflow rejection and image classification. Full x86/x64 builds and rendering/SDK checks accompany the candidate.

This is an experimental capacity fix. Eight images are a bounded test target, not a guarantee for every game's recording depth. Live testing must establish whether it resolves Warframe's flicker within the reporter's memory budget before stable promotion.

File version remains **1.7.3.680**. The overlay identifies **Scene input preview 6 (pre-release)** and `BUILD-INFO.txt` records the source commit.
