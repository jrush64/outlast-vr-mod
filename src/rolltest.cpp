// Verify the yaw/pitch/roll extraction against a KNOWN composed rotation.
// Composition order must match the decomposition: R = Ry(yaw) * Rx(pitch) * Rz(roll).
#include <cstdio>
#include <cmath>

struct Q { float x, y, z, w; };

static Q QMul(const Q& a, const Q& b) {
    return { a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
             a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
             a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
             a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z };
}
static Q AxisY(float a) { return { 0, sinf(a*0.5f), 0, cosf(a*0.5f) }; }
static Q AxisX(float a) { return { sinf(a*0.5f), 0, 0, cosf(a*0.5f) }; }
static Q AxisZ(float a) { return { 0, 0, sinf(a*0.5f), cosf(a*0.5f) }; }

static const float D = 57.29577951f;

int main() {
    struct { float yaw, pitch, roll; } cases[] = {
        {   0,   0,   0 }, {  45,   0,   0 }, {  90,   0,   0 },
        { 120,   0,   0 }, { 179,   0,   0 }, { -150,  0,   0 },   // looking behind
        {   0,  20,   0 }, {   0, -30,   0 }, {   0,   0,  25 },
        { 150,  20,  10 }, { -170, -15, -20 }, { 100,  40,  30 },
    };
    printf("  input yaw/pitch/roll   ->        OLD roll     NEW roll   (yaw,pitch read back)\n");
    bool allOk = true;
    for (auto& c : cases) {
        Q q = QMul(QMul(AxisY(c.yaw/D), AxisX(c.pitch/D)), AxisZ(c.roll/D));
        float fx = -2.0f * (q.x*q.z + q.w*q.y);
        float fy = -2.0f * (q.y*q.z - q.w*q.x);
        float fz = -(1.0f - 2.0f * (q.x*q.x + q.y*q.y));
        float pyc = fy < -1.f ? -1.f : (fy > 1.f ? 1.f : fy);
        float yaw   = atan2f(fx, -fz) * D;      // code returns the NEGATED xr yaw, by design
        float pitch = asinf(pyc) * D;
        float oldRoll = atan2f(2.f*(q.w*q.z + q.x*q.y), 1.f - 2.f*(q.y*q.y + q.z*q.z)) * D;
        float newRoll = atan2f(2.f*(q.x*q.y + q.w*q.z), 1.f - 2.f*(q.x*q.x + q.z*q.z)) * D;
        bool ok = fabsf(newRoll - c.roll) < 0.05f &&
                  fabsf(pitch  - c.pitch) < 0.05f &&
                  fabsf(yaw    + c.yaw)   < 0.05f;   // yaw is returned negated
        if (!ok) allOk = false;
        printf("%7.1f %6.1f %6.1f   ->  %10.1f  %10.1f   (%7.1f %6.1f)  %s\n",
               c.yaw, c.pitch, c.roll, oldRoll, newRoll, yaw, pitch, ok ? "ok" : "MISMATCH");
    }
    printf("\n%s\n", allOk ? "ALL CASES RECOVER THE INPUT" : "*** FORMULA STILL WRONG ***");

    // ---- roll strip: head tilt must change NOTHING on screen ------------------------------
    // The declared pose is rebuilt as Ry(yaw)*Rx(pitch). Verify that discards roll exactly and
    // leaves yaw and pitch untouched - if it perturbed either, cancelling roll would drag the
    // view sideways on every head tilt.
    printf("\nroll strip: rebuild Ry(yaw)*Rx(pitch) from the extracted angles\n");
    printf("  input yaw/pitch/roll   ->   recovered yaw/pitch/roll\n");
    bool stripOk = true;
    for (auto& c : cases) {
        Q q = QMul(QMul(AxisY(c.yaw/D), AxisX(c.pitch/D)), AxisZ(c.roll/D));
        float fx = -2.0f * (q.x*q.z + q.w*q.y);
        float fy = -2.0f * (q.y*q.z - q.w*q.x);
        float fz = -(1.0f - 2.0f * (q.x*q.x + q.y*q.y));
        float pyc = fy < -1.f ? -1.f : (fy > 1.f ? 1.f : fy);
        float yawRead = atan2f(fx, -fz);      // negated xr yaw, as the code stores it
        float pitchRead = asinf(pyc);

        // exactly what ol_xr.cpp now builds for the declared pose
        float hy = -yawRead * 0.5f, hp = pitchRead * 0.5f;
        Q qy = { 0, sinf(hy), 0, cosf(hy) }, qx = { sinf(hp), 0, 0, cosf(hp) };
        Q s = QMul(qy, qx);

        float sfx = -2.0f * (s.x*s.z + s.w*s.y);
        float sfy = -2.0f * (s.y*s.z - s.w*s.x);
        float sfz = -(1.0f - 2.0f * (s.x*s.x + s.y*s.y));
        float spyc = sfy < -1.f ? -1.f : (sfy > 1.f ? 1.f : sfy);
        float sYaw   = atan2f(sfx, -sfz) * D;
        float sPitch = asinf(spyc) * D;
        float sRoll  = atan2f(2.f*(s.x*s.y + s.w*s.z), 1.f - 2.f*(s.x*s.x + s.z*s.z)) * D;

        bool ok = fabsf(sRoll) < 0.05f &&
                  fabsf(sPitch - c.pitch) < 0.05f &&
                  fabsf(sYaw + c.yaw) < 0.05f;
        if (!ok) stripOk = false;
        printf("%7.1f %6.1f %6.1f   ->  %7.1f %6.1f %6.1f   %s\n",
               c.yaw, c.pitch, c.roll, -sYaw, sPitch, sRoll, ok ? "ok" : "MISMATCH");
    }
    printf("\n%s\n", stripOk ? "ROLL FULLY REMOVED, YAW AND PITCH PRESERVED"
                             : "*** ROLL STRIP PERTURBS THE VIEW ***");
    return (allOk && stripOk) ? 0 : 1;
}
