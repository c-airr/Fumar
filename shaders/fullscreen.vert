#version 450

// A triangle that covers the screen, built with no vertex buffer at all.
//
// The obvious way to cover the screen is two triangles forming a quad. One
// oversized triangle is better: there is no shared edge down the middle, where
// the GPU would shade the same pixels twice (fragments are shaded in 2x2 groups
// that straddle the seam), and it needs three vertices instead of six.
//
// gl_VertexIndex 0,1,2 produce (0,0), (2,0), (0,2) in UV, which is (-1,-1),
// (3,-1), (-1,3) in clip space. Everything outside the screen is clipped away
// and what is left is exactly the visible area.

layout(location = 0) out vec2 vUV;

void main() {
    vUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);

    // z = 1 puts the triangle on the far plane, so any geometry drawn later
    // passes the depth test against it. The sky pass does not write depth
    // anyway, but this keeps it correct if that ever changes.
    gl_Position = vec4(vUV * 2.0 - 1.0, 1.0, 1.0);
}
