#pragma once
// Snake's 178-bone animation pose, model space, published every frame into PoseBlock.
//
// SkinPublish(driver, binding) 0x141A6CAA0 runs for every skinned model (research §7.5). The camera
// frame resolves which driver/binding pair is the player's (owner+0x370 -> character -> component ->
// holder -> model/driver, all vtable-checked, as MGS5VR controller_rig.cpp does); the hook publishes
// only that pair, after the engine's own publication.

#include "Logic.h"

#include <cstdint>
#include <string>

namespace phantom::guest::skeleton
{
	bool Install();
	// Game thread, from the camera frame: (re)resolve the player's skin target from the owner.
	void ObserveOwner(std::uintptr_t a_owner, std::uint64_t a_frame);

	// Latest world-space position of one bone (for muzzles); false if the pose is stale (>200 ms).
	bool BoneWorld(int a_bone, float a_out[3]);
	[[nodiscard]] std::uint64_t PublishedFrames();

	// The deployed buddy's own skin (D-Horse, D-Dog, Quiet: no GetPosition), learned near a hint: Snake's root while he rides
	// it, where boot called it, beside him after a whistle (docs/GUEST.md 8.1). Any thread.
	void HintBuddy(const logic::Vec3& a_at, float a_radius, std::uint64_t a_forMs, bool a_horizontal = true);
	void ForgetBuddy(const char* a_why);
	// Its model root's position and heading from the last publication, if not older than a_maxAgeMs.
	bool BuddyPose(logic::Vec3& a_pos, float& a_yaw, std::uint64_t a_maxAgeMs = 500);
	std::string DescribeBuddy();
}
