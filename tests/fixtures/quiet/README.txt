Quiet / grace-rest fixtures (docs/research/quiet.md §2.9, §3.2, §3.9, §9 I0)
==========================================================================

Recorded live on 2026-11-04 (06:59-07:26) in the Quiet design study's Elden Ring session: Elden Ring 1.17.2 (exe 2.7.2.0,
--er-headless 3840x2160, offline, the exclusive virtual pad) with fake_mgsv --buddy quiet and a replay of the real Snake. Six rests
at three graces (Rampart Tower, Gatefront, The First Step), none with an NPC talk. The save was restored afterwards (sha256-equal).
Copied unchanged from the study's scratch folder; they hold positions, ray verdicts, enemy param ids / teams and frame luminance
only (no paths, no Steam ids, no names). They feed the host's pure GraceLogic / QuietLogic tests as an injected ray oracle.

Coordinates: Elden Ring physics (Havok) space of that moment, metres, Y up. Elden Ring moves its origin (world shifts), so the
numbers are only comparable within one file. "deg" is the candidate's bearing round the centre, measured from +X toward +Z
(x = cx + r cos(deg), z = cz + r sin(deg)). Rays: "call rayall" / "call ray ... 0x2000016" (the ground filter characters stand on)
through the host console. Layers are Havok collision layers as hex strings.

survey_rampart.json         Sit-spot survey round the Rampart Tower grace (AEG099_060_9005; wood floor and rugs indoors).
survey_gatefront.json       The same at Gatefront (dirt and grass, a low stone border, a slope west).
survey_firststep_real.json  The same at The First Step, with the real MGSV camera of the replay (level, ~5 m behind Snake).
    Top level: label; G = the grace asset (x, y, z); gfloor = the floor under the grace (upward normal); S = Snake's (x, z) at the
    rest; cam = the rest camera (pos, fwd, right, up) and fov (vertical, rad; 16:9); secs = survey time.
    spots[64] = rings r 1.5 / 2.0 / 2.5 / 3.0 m x 16 bearings round G. Per spot: x, z; h = the visible surface (ray filter) and its
    normal ny and layer; dh = h - gfloor; h_groundfilter = the ground-filter floor; foot_spread = the 4 footprint rays' spread
    (+-0.35 m); headroom_hit (1.3 m up); walls8 = knee rays (0.5 m up, 0.45 m out) that hit; reach_blocked = the grace -> spot line
    at 0.5 m; snake_d = m from Snake; sx, sy = screen position (0..1) under the rest camera, cz = its depth; cam_los_blocked; behind_snake
    (inside his silhouette); face_grace_yaw_deg = the yaw that faces the grace. A spot without ground has "fail" instead.
    The study's rule (quiet.md §3.9) picked r 2.0 m @68 deg at Rampart and Gatefront and r 2.0 m @45 deg at The First Step.

perch_gatefront.json        Sniper-post survey round Snake at Gatefront (7 hostiles within 60 m).
perch_firststep.json        The same at The First Step (a hilltop; 5 hostiles).
    Top level: label; S = Snake (x, y, z); enemies = hostile characters within 60 m (npc = NpcParam id, team = team type, hp = max
    HP, p = feet); rays = rays cast; secs.
    spots[120] = rings r 20 / 30 / 40 / 50 / 60 m x 24 bearings round Snake. Per spot: x, z; gy = standable ground (ground filter,
    from 70 m above), dy = gy - Snake's y, ny and layer of that ground; cover_above = the first surface seen from above minus gy
    (canopy, roof); headroom (1.6 m clear); sees_snake (eye at gy + 0.8 m -> Snake's chest); seen / sees_enemies = the enemies whose
    chest the eye sees; nearest_enemy = m to the nearest hostile. A spot without ground has "fail" instead.

restlum_firststep.json      The rest's fade at The First Step: [seconds since the Y press, mean frame luminance (0-255, of a
    192x108 grey copy of "hud shot"), the host's status flags]. Normal to +0.24 s, fade-out to black by +1.0 s, black +1.0 to
    +2.5 s, fade-in to +3.0 s, then the grace menu (quiet.md §2.9; U12 decides the host's own black signal).

Recorded by the host itself in round E2 (2026-11-04, Elden Ring 1.17.2 with fake_mgsv --buddy quiet; every ray the code cast, so the
offline tests replay the same rules ray for ray: tests/test_host_grace.inc TestGraceRecorded, tests/test_host_quiet.inc
TestQuietRecorded). Positions, ray origins / deltas / hits and runtime character handles only.
    Common: "rays" = [{"f": origin, "d": delta, "k": 0 the ground filter | 1 the ray filter, "h": hit or null}], in the order cast.

grace_rays_gatefront.json   A rest at Gatefront (`grace record on`): the scene (grace centre from its AEG099_060 asset, the floor
    under it, Snake, the rest's view at the black's first frame, other characters near), the seat search's rays, its result
    (2.0 m @90 deg, screen x 0.61) -- the first candidate in order passed.
grace_rays_rampart_all.json `grace survey all rampart` at Rampart Tower: every one of the 64 candidates looked at ("all": true: the
    replay looks at all of them too), 582 rays, 22 passed.
quiet_rays_{gatefront,firststep}_{cover,near}.json  `quiet survey <place>` (U13): the design's post rule on every candidate of
    both modes (cover rings 25/35/45/55 m, near 8/12/16 m, 24 bearings) round Snake, the hostiles (feet, chest), the characters,
    the camera (60 deg vertical FOV: fake_mgsv's), the aim point; "spots" = each candidate's verdict, seen count and score.
    Gatefront: cover 13 of 96 passed, near 24 of 72; The First Step: cover 17 of 96, near 37 of 72 (at MGSV's own 35.6 deg FOV the
    test recounts: the cover passes are the same, near 11 / 23).
