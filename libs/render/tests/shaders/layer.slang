struct Push {
    float layer;
};

[[vk::push_constant]] ConstantBuffer<Push> push;
[[vk::binding(0, 0)]] Sampler2DArray layers;

struct Corner {
    float4 position : SV_Position;
    float2 at;
};

// One triangle covering the whole image, reading across the layer from its top left.
[shader("vertex")]
Corner vertex_main(uint id: SV_VertexID) {
    const float2 at = float2((id << 1) & 2, id & 2);
    Corner corner;
    corner.position = float4((at * 2.0) - 1.0, 0.5, 1.0);
    corner.at = at;
    return corner;
}

[shader("fragment")]
float4 fragment_main(Corner corner) : SV_Target {
    return layers.Sample(float3(corner.at, push.layer));
}
