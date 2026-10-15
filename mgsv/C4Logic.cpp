#include "C4Logic.h"

namespace phantom::guest::c4
{
	int PickOldest(std::span<const Candidate> a_all, std::uint16_t a_inHand)
	{
		int same = -1, any = -1;
		for (int i = 0; i < static_cast<int>(a_all.size()); ++i) {
			const auto& c = a_all[i];
			if (!c.id || c.setOff || !IsC4(c.equip)) {
				continue;
			}
			if (any < 0 || c.id < a_all[any].id) {
				any = i;
			}
			if (c.equip == a_inHand && (same < 0 || c.id < a_all[same].id)) {
				same = i;
			}
		}
		return same >= 0 ? same : any;
	}

	Detonator::Action Detonator::Step(const Input& a_in)
	{
		const bool down = a_in.mask != 0 && (a_in.buttons & a_in.mask) == a_in.mask;
		const bool edge = down && !m_down;
		m_down = down;
		if (edge && a_in.armed) {
			++m_presses;
			if (m_haveAnswer && a_in.ms >= m_lastAnswerMs && a_in.ms - m_lastAnswerMs <= kRecentMs) {
				// MGSV (or the guest at PlayerDetonate) already answered it earlier this frame.
				m_haveAnswer = false;
				++m_answered;
				m_pending = false;
			} else {
				m_pending = true;
				m_pressMs = a_in.ms;
			}
		}
		if (m_pending && !a_in.armed) {
			m_pending = false;  // the C4 went away (picked up, menu, death): nothing left to set off for this press
		}
		if (m_pending && down && a_in.ms - m_pressMs > kHoldMs) {
			m_pending = false;  // a hold: Fulton's or carrying's, never the detonator's (no fallback for it)
			++m_holds;
			return Action::kNone;
		}
		if (m_pending && !down && a_in.ms - m_pressMs >= m_graceMs) {
			m_pending = false;
			++m_fallbacks;
			return Action::kFallback;
		}
		return Action::kNone;
	}

	void Detonator::NoteAnswered(std::uint64_t a_ms)
	{
		if (m_pending) {
			m_pending = false;
			++m_answered;
			m_haveAnswer = false;
			return;
		}
		m_lastAnswerMs = a_ms;
		m_haveAnswer = true;
	}
}
