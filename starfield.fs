#version 330
in vec2 fragTexCoord;
out vec4 finalColor;
 
uniform vec2  resolution;
uniform float time;
 
// Camera basis — lets each pixel become a world-space direction
uniform vec3  camForward;
uniform vec3  camRight;
uniform vec3  camUp;
uniform float fovY;          // vertical field of view, in radians
 
float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}
 
void main() {
    // 1. Reconstruct this pixel's world-space ray direction from the camera.
    vec2 uv = (gl_FragCoord.xy / resolution) * 2.0 - 1.0;
    uv.x *= resolution.x / resolution.y;          // aspect correction
    float t = tan(fovY * 0.5);
    vec3 dir = normalize(camForward + uv.x * t * camRight + uv.y * t * camUp);
 
    // 2. Direction -> sky coordinates (longitude / latitude on the celestial sphere).
    float az = atan(dir.z, dir.x);                // -PI .. PI
    float el = asin(clamp(dir.y, -1.0, 1.0));     // -PI/2 .. PI/2
    vec2  sky = vec2(az, el);
 
    // 3. Same grid-cell star logic as before, but in sky coordinates now.
    vec2 grid = sky * vec2(40.0, 40.0);           // star density (tune)
    vec2 cell = floor(grid);
    vec2 frac = fract(grid);
 
    float rnd    = hash(cell);
    float isStar = step(0.985, rnd);
 
    float d    = length(frac - 0.5);
    float glow = smoothstep(0.25, 0.0, d);
 
    float twinkle = 0.6 + 0.4 * sin(time * 2.0 + rnd * 6.2831);
 
    float brightness = isStar * glow * twinkle;
    vec3  color = brightness * vec3(0.8, 0.9, 1.0);
    finalColor = vec4(color, 1.0);
}
