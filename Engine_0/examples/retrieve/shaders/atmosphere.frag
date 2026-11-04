#version 450

#define PI 3.1415926
#define RAYMARCH_STEP 16

layout(set = 0, binding = 3) uniform sampler2D TransmittanceLUT;
layout(set = 0, binding = 4) uniform sampler2D MultipleScatteringLUT;
layout(set = 0, binding = 5) uniform sampler2D SkyAmbientLUT;
layout(set = 0, binding = 6) uniform sampler2D canvas;
layout(set = 0, binding = 8) uniform sampler2D depth;

layout(std140, set = 0, binding = 0) uniform FrameUniformsBlock {
    mat4 PMat;
    mat4 PMat_iv;
    mat4 lightSpaceMatrix;
    vec4 dlight;
    vec2 resolution;
    float time;
    vec3 lightCol;
    vec3 cameraPos;
    vec3 lightDir;
} FrameUniforms;

layout(std140, set = 0, binding = 2) uniform AtmosphereParamsBlock {
    float H_Rayleigh;
    float H_Mie;
    float g_Mie;
    float PlanetRadius;
    float AtmosphereRadius;
    float H_Ozone;
    float W_Ozone;
    vec3 SunLuminance;
    vec3 PlanetCenter;
} AtmosphereParams;

#define PMat_iv FrameUniforms.PMat_iv
#define resolution FrameUniforms.resolution
#define cameraPos FrameUniforms.cameraPos
#define lightDir FrameUniforms.lightDir
#define H_Rayleigh AtmosphereParams.H_Rayleigh
#define H_Mie AtmosphereParams.H_Mie
#define PlanetRadius AtmosphereParams.PlanetRadius
#define AtmosphereRadius AtmosphereParams.AtmosphereRadius
#define H_Ozone AtmosphereParams.H_Ozone
#define W_Ozone AtmosphereParams.W_Ozone
#define SunLuminance AtmosphereParams.SunLuminance
#define PlanetCenter AtmosphereParams.PlanetCenter

layout(location = 0) out vec4 fragColor;

const vec3 MoonLuminance = 0.8 * vec3(1., 1., 1);

vec3 RayIntersectSphere(vec3 p, vec3 dir, vec3 center, float r) {
    vec3 oc = center - p;
    vec3 a = dot(oc, dir) * dir;
    float dis_sq = dot(oc, oc) - dot(a, a);
    if (dis_sq > r * r || (dot(oc, dir) < 0 && dot(oc, oc) > r * r)) return center;
    float b = sqrt(r * r - dis_sq);

    float delta = dot(oc, oc) - r * r;
    delta = 1.0 - 2.0 * step(0.0, delta);

    return p + a + b * dir * delta;
}

vec4 getGlobalPosition(vec4 ndc) {
    vec4 p = PMat_iv * ndc;
    return p / p.w;
}

vec3 RayleighCoeff(float h) {
    const vec3 sigma0 = vec3(5.802, 13.558, 33.1) * 1e-6;
    float rho_h = exp(-h / H_Rayleigh);
    return sigma0 * rho_h;
}

float RayleighPhase(float cos_theta) {
    return (3.0 / (16.0 * PI)) * (1.0 + cos_theta * cos_theta);
}

vec3 MieCoeff(float h) {
    const vec3 sigma0 = vec3(3.996) * 1e-6;
    float rho_m = exp(-h / H_Mie);
    return sigma0 * rho_m;
}

float MiePhase(float g, float cos_theta) {
    float a = 3. / 8. / PI;
    float b = (1 - g * g) * (1 + cos_theta * cos_theta);
    float c = (2 + g * g) * pow((1 + g * g - 2 * g * cos_theta), 1.5);
    return a * b / c;
}

vec3 Scattering_rgb(vec3 indir, vec3 p, vec3 outdir) {
    float cos_theta = dot(indir, outdir);
    float h = length(p - PlanetCenter) - PlanetRadius;
    return RayleighCoeff(h) * RayleighPhase(cos_theta) +
        MieCoeff(h) * MiePhase(.6, cos_theta);
}

vec3 MieAbsorbtion_rgb(float h) {
    const vec3 sigma0 = vec3(4.40) * 1e-6;
    float rho_m = exp(-h / H_Mie);
    return sigma0 * rho_m;
}

vec3 OzoneAbsorbtion_rgb(float h, float center, float width) {
    const vec3 sigma0 = vec3(0.650, 1.881, 0.085) * 1e-5;
    float rho_o = max(0., 1 - abs(h - center) / width);
    return sigma0 * rho_o;
}

vec2 param2uv(float r, float miu) {
    r = clamp(r, PlanetRadius, AtmosphereRadius);
    miu = clamp(miu, -1.0, 1.0);
    float u = (r - PlanetRadius) / (AtmosphereRadius - PlanetRadius);
    float v = (miu + 1.0) * 0.5;
    return vec2(u, v);
}

vec3 Transmittance(vec3 p1, vec3 p2) {
    vec3 diff = p2 - p1;
    if (dot(diff, diff) < 1e-6) return vec3(1.0);

    float r1 = length(p1 - PlanetCenter);
    float r2 = length(p2 - PlanetCenter);

    vec3 lower = (r1 <= r2) ? p1 : p2;
    vec3 upper = (r1 <= r2) ? p2 : p1;
    float rl = clamp(min(r1, r2), PlanetRadius, AtmosphereRadius);
    float ru = clamp(max(r1, r2), PlanetRadius, AtmosphereRadius);

    vec3 upd = upper - lower;
    vec3 updir = upd / max(length(upd), 1e-6);

    vec3 rad_l = lower - PlanetCenter;
    vec3 rad_u = upper - PlanetCenter;
    float len_l = length(rad_l);
    float len_u = length(rad_u);
    float mu_l = clamp(len_l > 1e-3 ? dot(rad_l / len_l, updir) : 1.0, -1.0, 1.0);
    float mu_u = clamp(len_u > 1e-3 ? dot(rad_u / len_u, updir) : 1.0, -1.0, 1.0);

    vec3 Tl = texture(TransmittanceLUT, param2uv(rl, mu_l)).xyz;
    vec3 Tu = texture(TransmittanceLUT, param2uv(ru, mu_u)).xyz;

    return Tl / max(Tu, vec3(1e-4));
}

vec3 getDirection(vec2 ndc) {
    vec4 posNear = vec4(ndc, 0.0, 1.0);
    vec4 posFar = vec4(ndc, 1.0, 1.0);
    posNear = PMat_iv * posNear;
    posFar = PMat_iv * posFar;
    posNear /= posNear.w;
    posFar /= posFar.w;
    return normalize(posFar - posNear).xyz;
}

vec3 intersectUV(vec3 pos, vec3 dir) {
    vec3 a = RayIntersectSphere(pos, dir, PlanetCenter, AtmosphereRadius);
    vec3 b = RayIntersectSphere(pos, dir, PlanetCenter, PlanetRadius);
    vec3 c = b - PlanetCenter;
    return dot(c, c) < 0.1 ? a : b;
}

vec3 theSun(vec3 dir, vec3 pos) {
    vec3 col = vec3(1.);
    col = col * (step(0.999, dot(dir, -lightDir)));
    vec3 t = Transmittance(pos, RayIntersectSphere(pos, dir, PlanetCenter, AtmosphereRadius));
    return col * t;
}

vec3 theMoon(vec3 dir, vec3 pos) {
    float cosAng = dot(dir, lightDir);
    float angRadius = 0.9998;
    float s = step(angRadius, cosAng);

    vec3 moonAlbedo = MoonLuminance;

    vec3 atmHit = RayIntersectSphere(pos, dir, PlanetCenter, AtmosphereRadius);
    vec3 tint = vec3(1.0);
    if (dot(atmHit - pos, atmHit - pos) > 1e-4)
        tint = Transmittance(pos, atmHit);

    return moonAlbedo * s * tint;
}

float height(vec3 pos) {
    return length(pos - PlanetCenter) - PlanetRadius;
}

vec3 get_ms(vec3 pos) {
    float r = length(pos - PlanetCenter);
    vec3 radialDir = normalize(pos - PlanetCenter);
    float miu = dot(normalize(-lightDir), radialDir);
    return texture(MultipleScatteringLUT, param2uv(r, miu)).rgb;
}

vec3 RayMarchUnit(vec3 pos, vec3 dir, vec3 L) {
    vec3 col = vec3(0.);
    vec3 p1 = pos;

    vec3 v_intersect = intersectUV(pos, dir);
    vec3 b = v_intersect - PlanetCenter;
    if (dot(b, b) < 0.1) return vec3(0.);

    float ds = length(v_intersect - pos) / float(RAYMARCH_STEP);

    for (int i = 0; i < RAYMARCH_STEP; ++i) {
        vec3 p2 = RayIntersectSphere(p1, -L, PlanetCenter, AtmosphereRadius);

        vec3 t1 = Transmittance(p2, p1);
        vec3 s = Scattering_rgb(L, p1, -dir);
        vec3 t2 = Transmittance(p1, pos);

        col += t1 * s * t2 * ds;

        p1 += dir * ds;
    }

    return col;
}

vec3 RayMarchMS(vec3 pos, vec3 dir) {
    vec3 col = vec3(0.);
    vec3 p1 = pos;

    vec3 v_intersect = intersectUV(pos, dir);
    vec3 b = v_intersect - PlanetCenter;
    if (dot(b, b) < 0.1) return vec3(0.);

    float ds = length(v_intersect - pos) / float(RAYMARCH_STEP);

    for (int i = 0; i < RAYMARCH_STEP; ++i) {
        vec3 t2 = Transmittance(p1, pos);

        vec3 multiScattering = get_ms(p1);
        vec3 sigma = MieCoeff(height(p1)) + RayleighCoeff(height(p1));

        col += multiScattering * t2 * sigma * ds;

        p1 += dir * ds;
    }

    return col;
}

void main() {
    vec2 uv = gl_FragCoord.xy / resolution;
    float dep = texture(depth, uv).r;

    if (dep >= 0.9999) {
        vec2 ndc = uv * 2. - 1.;
        vec3 dir = getDirection(ndc);
        vec3 col = RayMarchUnit(cameraPos, dir, lightDir) * SunLuminance
                 + RayMarchUnit(cameraPos, dir, -lightDir) * MoonLuminance
                 + RayMarchMS(cameraPos, dir) * SunLuminance
                 + RayMarchMS(cameraPos, dir) * MoonLuminance;
        col = tanh(col * 0.5);
        col += theSun(dir, cameraPos) + theMoon(dir, cameraPos);
        fragColor = vec4(col, 1.);
    }
    else {
        float albedo = 0.9;
        vec4 ndc = vec4(uv * 2. - 1., dep, 1.);
        vec3 pos = getGlobalPosition(ndc).xyz;
        vec3 radialDir = normalize(pos - PlanetCenter);
        vec3 T = texture(TransmittanceLUT,
            param2uv(length(pos - PlanetCenter), dot(radialDir, -lightDir))).xyz;
        vec3 direct_luminance = T * SunLuminance;

        float NdotMoon = clamp(dot(radialDir, normalize(lightDir)), 0.0, 1.0);
        vec3 Tmoon = texture(TransmittanceLUT,
            param2uv(length(pos - PlanetCenter), dot(radialDir, lightDir))).xyz;
        vec3 moon_luminance = Tmoon * MoonLuminance * NdotMoon;

        vec3 ambient_luminance = texture(SkyAmbientLUT,
            param2uv(length(pos - PlanetCenter), dot(radialDir, -lightDir))).xyz
            * SunLuminance;
        vec3 moon_ambient_luminance = texture(SkyAmbientLUT,
            param2uv(length(pos - PlanetCenter), dot(radialDir, lightDir))).xyz
            * MoonLuminance;

        vec4 canvasData = texture(canvas, uv);

        vec3 fromSun = canvasData.rrr * direct_luminance + ambient_luminance * canvasData.z;
        vec3 fromMoon = canvasData.rrr * moon_luminance + moon_ambient_luminance * canvasData.z;

        vec3 color = albedo * (tanh(fromSun * 0.1) + tanh(fromMoon * 0.2));
        fragColor = vec4(color, 1.);
    }
}
