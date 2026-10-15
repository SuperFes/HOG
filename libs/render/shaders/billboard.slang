struct Push {
    float4x4 clip_from_eye;
    // xyz the feet from the eye, in metres.
    float4 feet;
    // xyz from the picture's middle to its right edge, and from its foot to its top.
    float4 right;
    float4 up;
};

[[vk::push_constant]] ConstantBuffer<Push> push;
[[vk::binding(0, 0)]] Sampler2D picture;

struct Corner {
    float4 position : SV_Position;
    float2 uv;
};

[shader("vertex")]
Corner vertex_main(uint index : SV_VertexID) {
    // Two triangles, as x from -1 to 1 across and y from 0 to 1 up.
    static const float2 corners[6] = {float2(-1.0, 0.0), float2(1.0, 0.0), float2(1.0, 1.0),
                                      float2(-1.0, 0.0), float2(1.0, 1.0), float2(-1.0, 1.0)};
    const float2 at = corners[index];
    Corner corner;
    const float3 offset = push.feet.xyz + (push.right.xyz * at.x) + (push.up.xyz * at.y);
    corner.position = mul(push.clip_from_eye, float4(offset, 1.0));
    corner.uv = float2((at.x + 1.0) / 2.0, 1.0 - at.y);
    return corner;
}

[shader("fragment")]
float4 fragment_main(Corner corner) : SV_Target {
    const float4 texel = picture.Sample(corner.uv);
    if (texel.a < 0.5) {
        discard;
    }
    return float4(texel.rgb, 1.0);
}
