// Prints the protocol layout as JSON so tools/phantomring/proto.py can be checked against it
// (tools/phantomring/test_layout.py runs this under wine and compares).
#include "phantomring_protocol.h"

#include <cstdio>

using namespace phantom::proto;

#define OFF(T, m) std::printf("  \"%s.%s\": %zu,\n", #T, #m, offsetof(T, m))
#define SIZE(T) std::printf("  \"sizeof(%s)\": %zu,\n", #T, sizeof(T))
#define CONST(c) std::printf("  \"%s\": %llu,\n", #c, static_cast<unsigned long long>(c))

int main()
{
	std::printf("{\n");
	CONST(kVersion);
	CONST(kOffHeader);
	CONST(kOffHostState);
	CONST(kOffGuestState);
	CONST(kOffPad);
	CONST(kOffPose);
	CONST(kOffGuestEvents);
	CONST(kOffHostEvents);
	CONST(kOffActors);
	CONST(kOffGuestConsole);
	CONST(kOffHostConsole);
	CONST(kOffDrawCtl);
	CONST(kOffHudCtl);
	CONST(kOffRenderRing);
	CONST(kRenderRingBytes);
	CONST(kOffDrawSlots);
	CONST(kDrawSlotBytes);
	CONST(kOffHudSlots);
	CONST(kHudSlotBytes);
	CONST(kMappingBytes);
	CONST(kPlacedAgain);
	SIZE(Header);
	OFF(Header, sessionNonce);
	OFF(Header, guestNonce);
	OFF(Header, hostUnixPid);
	OFF(Header, guestUnixPid);
	OFF(Header, qpcFrequency);
	OFF(Header, hostHeartbeatQpc);
	OFF(Header, guestHeartbeatQpc);
	OFF(Header, hostStatus);
	OFF(Header, guestStatus);
	OFF(Header, hostStatusText);
	OFF(Header, guestStatusText);
	OFF(Header, guestProtocol);
	OFF(Header, hostRelease);
	OFF(Header, guestRelease);
	SIZE(HostState);
	OFF(HostState, flags);
	OFF(HostState, frame);
	OFF(HostState, qpc);
	OFF(HostState, timeOfDayHours);
	OFF(HostState, cameraMaxDistance);
	OFF(HostState, erHpFrac);
	OFF(HostState, erWorldId);
	OFF(HostState, groundY);
	OFF(HostState, fallSpeed);
	SIZE(GuestState);
	OFF(GuestState, flags);
	OFF(GuestState, frame);
	OFF(GuestState, qpc);
	OFF(GuestState, missionCode);
	OFF(GuestState, locationCode);
	OFF(GuestState, odoEpoch);
	OFF(GuestState, odo);
	OFF(GuestState, rootPos);
	OFF(GuestState, rootYaw);
	OFF(GuestState, velocity);
	OFF(GuestState, camPos);
	OFF(GuestState, camQuat);
	OFF(GuestState, camFovY);
	OFF(GuestState, camNear);
	OFF(GuestState, life);
	OFF(GuestState, lifeMax);
	OFF(GuestState, stamina);
	OFF(GuestState, stance);
	OFF(GuestState, weaponId);
	OFF(GuestState, ammoInClip);
	OFF(GuestState, ammoStock);
	OFF(GuestState, buddyType);
	OFF(GuestState, vehicleId);
	OFF(GuestState, phase);
	OFF(GuestState, buddyFlags);
	OFF(GuestState, buddyObjectId);
	OFF(GuestState, buddyAmmo);
	OFF(GuestState, buddyPos);
	OFF(GuestState, buddyYaw);
	OFF(GuestState, aimThrowPos);  // v6
	OFF(GuestState, aimThrowVel);
	OFF(GuestState, aimThrowKind);
	SIZE(PadState);
	OFF(PadState, flags);
	OFF(PadState, packet);
	OFF(PadState, buttons);
	OFF(PadState, leftTrigger);
	OFF(PadState, rightTrigger);
	OFF(PadState, thumbLX);
	OFF(PadState, thumbLY);
	OFF(PadState, thumbRX);
	OFF(PadState, thumbRY);
	SIZE(PoseBlock);
	OFF(PoseBlock, boneCount);
	OFF(PoseBlock, frame);
	OFF(PoseBlock, root);
	OFF(PoseBlock, parent);
	OFF(PoseBlock, bones);
	SIZE(GuestEvent);
	SIZE(HostEvent);
	SIZE(ActorRecord);
	OFF(ActorRecord, flags);
	OFF(ActorRecord, pos);
	OFF(ActorRecord, yaw);
	OFF(ActorRecord, radius);
	OFF(ActorRecord, height);
	OFF(ActorRecord, hpFrac);
	OFF(ActorRecord, name);
	OFF(ActorTable, actors);
	SIZE(Console);
	OFF(Console, respSeq);
	OFF(Console, status);
	OFF(Console, request);
	OFF(Console, response);
	SIZE(DrawRecord);
	SIZE(DrawList);
	SIZE(RenMesh);
	SIZE(RenTexture);
	SIZE(RenInputElement);
	SIZE(HudSlotHdr);
	std::printf("  \"end\": 0\n}\n");
	return 0;
}
