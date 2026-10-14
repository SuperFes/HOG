struct Corner {
    float4 position : SV_Position;
    float3 colour;
};

// A red triangle over the middle of the image, drawn without vertex buffers.
[shader("vertex")]
Corner vertex_main(uint id: SV_VertexID) {
    const float2 corners[3] = { float2(0.0, -0.6), float2(0.6, 0.6), float2(-0.6, 0.6) };
    Corner corner;
    corner.position = float4(corners[id], 0.5, 1.0);
    corner.colour = float3(1.0, 0.0, 0.0);
    return corner;
}

[shader("fragment")]
float4 fragment_main(Corner corner) : SV_Target {
    return float4(corner.colour, 1.0);
}
