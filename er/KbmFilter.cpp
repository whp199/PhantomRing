#include "KbmFilter.h"

namespace phantom::er::Kbm
{
	DikTables DikTables::Build(const Params& a_params, int (*a_dik)(int))
	{
		DikTables t;
		const auto mark = [&](const std::vector<int>& a_vks, std::array<std::uint8_t, 256>& a_set, int* a_first) {
			for (const int vk : a_vks) {
				const int d = a_dik(vk);
				if (d < 0 || d > 0xFF) {
					continue;
				}
				a_set[static_cast<std::size_t>(d)] = 1;
				if (a_first && *a_first < 0) {
					*a_first = d;
				}
			}
		};
		mark(a_params.erKeys, t.er, nullptr);
		mark(a_params.erKeys, t.interaction, nullptr);
		mark(a_params.erInteractionKeys, t.interaction, nullptr);
		mark(a_params.keys[static_cast<std::size_t>(Action::kChord)], t.chord, &t.tap);
		return t;
	}

	bool ErKeeps(const DikTables& a_tables, int a_dik, Mode a_mode)
	{
		if (a_dik < 0 || a_dik > 0xFF) {
			return true;
		}
		const auto d = static_cast<std::size_t>(a_dik);
		switch (a_mode.scope) {
		case Scope::kFull:
			return !a_mode.guestMenu && a_tables.er[d];
		case Scope::kButtonsOnly:
			return a_tables.interaction[d] != 0;
		case Scope::kChordOnly:
			return !a_tables.chord[d];
		case Scope::kNone:
		default:
			return true;
		}
	}

	void SnapshotFilter::Keyboard(std::uint8_t* a_keys, const DikTables& a_tables, Mode a_mode, bool a_tap)
	{
		for (std::size_t i = 0; i < keys_.size(); ++i) {
			const bool held = a_keys[i] != 0;
			const bool hide = held && (keys_[i] || !ErKeeps(a_tables, static_cast<int>(i), a_mode));
			keys_[i] = hide;
			if (hide) {
				a_keys[i] = 0;
			}
		}
		if (a_tap && a_tables.tap >= 0 && a_tables.tap <= 0xFF) {
			a_keys[static_cast<std::size_t>(a_tables.tap)] = 0x80;
		}
	}

	bool SnapshotFilter::Mouse(std::uint8_t* a_buttons, std::size_t a_count, Scope a_scope)
	{
		const bool kept = ErGetsMouse(a_scope);
		for (std::size_t i = 0; i < a_count && i < buttons_.size(); ++i) {
			const bool held = a_buttons[i] != 0;
			const bool hide = held && (buttons_[i] || !kept);
			buttons_[i] = hide;
			if (hide) {
				a_buttons[i] = 0;
			}
		}
		return kept;
	}

	bool EventFilter::Pass(std::size_t a_index, bool a_down, bool a_kept)
	{
		if (a_index >= seen_.size()) {
			return true;
		}
		if (a_down) {
			if (!a_kept) {
				return false;
			}
			seen_[a_index] = true;
			return true;
		}
		// A release: whenever Elden Ring had the press. A kept key's passes too (one pressed before we counted is harmless
		// up; a missing one would stick).
		const bool pass = seen_[a_index] || a_kept;
		seen_[a_index] = false;
		return pass;
	}
}
