module flora;

// The dice a plant rolls from its own hash, as flora::hashed and flora::unit roll them.

public uint hashed(uint at) {
    at ^= at >> 16;
    at *= 0x7feb352du;
    at ^= at >> 15;
    at *= 0x846ca68bu;
    at ^= at >> 16;
    return at;
}

public float unit(uint at) {
    return float(hashed(at) & 0xffffu) / 65536.0;
}
