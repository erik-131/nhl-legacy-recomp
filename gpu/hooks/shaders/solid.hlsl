// High-cut C-3a: a solid-color pixel shader with NO inputs, paired with a TRANSLATED Xenos
// vertex shader to build a real plume graphics pipeline (defer textures / real PS translation).
// A zero-input PS is interface-compatible with any VS output set (Vulkan permits a fragment
// shader to consume fewer outputs than the VS produces), so it links against the Xenos VS that
// writes only gl_Position. Compiled to SPIR-V via dxc by plume's shader cmake -> solidFragBlobSPIRV.

// C-3b.2 objective verification (bypasses plume's blocked texture-readback path): a host-visible
// UAV the PS atomically increments per fragment. After a frame, the count == pixels the translated
// Xenos draw rasterized — proof the VS produced on-screen geometry, not just a validation-clean draw.
RWByteAddressBuffer xe_frag_counter : register(u0, space2);

float4 PSMain() : SV_Target {
    xe_frag_counter.InterlockedAdd(0, 1);
    return float4(1.0f, 0.3f, 0.1f, 1.0f);  // unmistakable solid orange
}
