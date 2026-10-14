struct Push {
    float4 colour;
    float depth;
};

[[vk::push_constant]] ConstantBuffer<Push> push;

// A triangle covering the whole image at one depth, in one colour.
[shader("vertex")]
float4 vertex_main(uint id: SV_VertexID) : SV_Position {
    const float2 corner = float2((id << 1) & 2, id & 2);
    return float4(corner * 2.0 - 1.0, push.depth, 1.0);
}

[shader("fragment")]
float4 fragment_main() : SV_Target {
    return push.colour;
}
