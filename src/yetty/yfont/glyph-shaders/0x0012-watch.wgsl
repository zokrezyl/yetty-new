// local_id 0x0012 — watch

const WG_PI: f32 = 3.14159265;
const WG_TAU: f32 = 6.28318530;

// --- local SDF helpers (from Inigo Quilez) ---

fn wg_sdCircle(p: vec2<f32>, r: f32) -> f32 {
    return length(p) - r;
}

fn wg_sdSegment(p: vec2<f32>, a: vec2<f32>, b: vec2<f32>) -> f32 {
    let pa = p - a;
    let ba = b - a;
    let h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return length(pa - ba * h);
}

// Rotate a 2D point by angle
fn wg_rot(p: vec2<f32>, a: f32) -> vec2<f32> {
    let c = cos(a);
    let s = sin(a);
    return vec2<f32>(c * p.x - s * p.y, s * p.x + c * p.y);
}

// Render a tapered hand: thick at base, thin at tip
fn wg_hand(p: vec2<f32>, angle: f32, len: f32, baseW: f32, tipW: f32) -> f32 {
    let rp = wg_rot(p, -angle);
    let t = clamp(rp.y / len, 0.0, 1.0);
    let w = mix(baseW, tipW, t);
    let dx = abs(rp.x) - w;
    let dy_lo = -rp.y;
    let dy_hi = rp.y - len;
    let dy = max(dy_lo, dy_hi);
    let outside = length(max(vec2<f32>(dx, dy), vec2<f32>(0.0)));
    let inside = min(max(dx, dy), 0.0);
    return outside + inside;
}

fn shader_glyph_18(local_uv: vec2<f32>, time: f32, fg: vec3<f32>, bg: vec3<f32>, pixel_pos: vec2<f32>) -> vec3<f32> {
    // Center at origin, range roughly -1..1, flip Y so +Y is up
    var p = (local_uv - 0.5) * 2.2;
    p.y = -p.y;

    var col = bg;

    // --- Hour markers ---
    for (var i = 0u; i < 12u; i++) {
        let a = f32(i) * WG_TAU / 12.0;
        let dir = vec2<f32>(sin(a), cos(a));
        let isMain = (i % 3u == 0u);
        let innerR = select(0.72, 0.65, isMain);
        let outerR = 0.82;
        let tickW = select(0.012, 0.022, isMain);
        let pa = dir * innerR;
        let pb = dir * outerR;
        let dTick = wg_sdSegment(p, pa, pb) - tickW;
        let tickAlpha = 1.0 - smoothstep(-0.008, 0.008, dTick);
        col = mix(col, fg, tickAlpha);
    }

    // --- Minute sub-markers ---
    for (var i = 0u; i < 60u; i++) {
        if (i % 5u != 0u) {
            let a = f32(i) * WG_TAU / 60.0;
            let dir = vec2<f32>(sin(a), cos(a));
            let dDot = wg_sdCircle(p - dir * 0.78, 0.008);
            let dotAlpha = 1.0 - smoothstep(-0.005, 0.005, dDot);
            col = mix(col, fg * 0.6, dotAlpha);
        }
    }

    // --- Clock hands ---
    let totalSec = time;
    let sec = totalSec % 60.0;
    let mn = (totalSec / 60.0) % 60.0;
    let hr = (totalSec / 3600.0) % 12.0;

    let secAngle = sec * WG_TAU / 60.0;
    let minAngle = mn * WG_TAU / 60.0;
    let hrAngle = hr * WG_TAU / 12.0;

    // Hour hand - short and thick
    let dHour = wg_hand(p, hrAngle, 0.45, 0.028, 0.012);
    let hourAlpha = 1.0 - smoothstep(-0.006, 0.006, dHour);
    col = mix(col, fg, hourAlpha);

    // Minute hand - longer and medium
    let dMin = wg_hand(p, minAngle, 0.68, 0.022, 0.008);
    let minAlpha = 1.0 - smoothstep(-0.006, 0.006, dMin);
    col = mix(col, fg, minAlpha);

    // Second hand - thin, with counterweight
    let secDir = vec2<f32>(sin(secAngle), cos(secAngle));
    let secTip = secDir * 0.75;
    let secTail = -secDir * 0.18;
    let dSec = wg_sdSegment(p, secTail, secTip) - 0.006;
    let secAlpha = 1.0 - smoothstep(-0.005, 0.005, dSec);
    let secColor = vec3<f32>(0.85, 0.12, 0.1);
    col = mix(col, secColor, secAlpha);

    // Second hand counterweight (small circle at tail)
    let dTail = wg_sdCircle(p - secTail, 0.03);
    let tailAlpha = 1.0 - smoothstep(-0.005, 0.005, dTail);
    col = mix(col, secColor, tailAlpha);

    // --- Center pin ---
    let dPin = wg_sdCircle(p, 0.035);
    let pinAlpha = 1.0 - smoothstep(-0.005, 0.005, dPin);
    col = mix(col, fg, pinAlpha);

    let dPinInner = wg_sdCircle(p, 0.018);
    let pinInnerAlpha = 1.0 - smoothstep(-0.005, 0.005, dPinInner);
    col = mix(col, bg, pinInnerAlpha);

    return col;
}
