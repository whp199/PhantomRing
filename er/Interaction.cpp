#include "Interaction.h"

namespace phantom::er
{
	bool Interaction::Update(const Input& a_in)
	{
		if (a_in.pressed && a_in.inWorld && !a_in.dead) {
			if (!active_) {
				sawHidden_ = false;
			}
			active_ = true;
			start_ = a_in.nowMs;  // a second press (leaving a menu, another pickup) restarts the minimum
		}
		if (!active_) {
			return false;
		}
		sawHidden_ = sawHidden_ || a_in.hudHidden;
		const std::uint64_t age = a_in.nowMs >= start_ ? a_in.nowMs - start_ : 0;
		if (!a_in.inWorld || a_in.dead || a_in.abort || (a_in.maxMs && age >= a_in.maxMs)) {
			active_ = false;  // a fast travel from the grace map (load), a death, the cap, or no screen after all
		} else if (age >= a_in.minMs && !a_in.hudHidden && !a_in.menuOpen) {
			active_ = false;
		}
		return active_;
	}

	bool AutoHud::Update(const Input& a_in)
	{
		if (detected_ || !a_in.hid || a_in.explained) {
			return false;
		}
		detected_ = ++unexplained_ >= kAutoHudHides;
		return detected_;
	}
}
