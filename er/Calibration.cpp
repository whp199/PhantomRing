#include "Calibration.h"

#include "Config.h"
#include "Log.h"
#include "Mem.h"

#include <cmath>
#include <format>
#include <numbers>

namespace phantom::er::Calibration
{
	namespace
	{
		struct Vote
		{
			int  plus{ 0 }, minus{ 0 };
			int  locked{ 0 };
			void Add(bool a_plus, const char* a_what)
			{
				if (locked) {
					return;
				}
				(a_plus ? plus : minus)++;
				const int total = plus + minus;
				if (total >= 40) {
					const int  lead = plus > minus ? plus : minus;
					const bool clear = lead * 10 >= total * 8;
					if (clear) {
						locked = plus > minus ? 1 : -1;
						Log::Info("calibration: {} = {:+d} ({} vs {} votes)", a_what, locked, plus, minus);
					} else if (total >= 400) {
						plus = minus = 0;  // no consensus yet: start over rather than lock on noise
					}
				}
			}
		};

		Vote  g_erForward;
		Vote  g_camRow;
		float g_handedness{ 0 };
		Vec3  g_lastPos;
		bool  g_havePos{ false };
		bool  g_loggedCam{ false };

		Vec3 Row(const std::uint8_t* a_cam, int a_row)
		{
			const auto* f = reinterpret_cast<const float*>(a_cam + game::kCamMatrix + 16 * a_row);
			return { f[0], f[1], f[2] };
		}
	}

	void Observe(const game::Player& a_player, float a_dt)
	{
		const Vec3 pos = game::Position(a_player);
		if (g_havePos && a_dt > 1e-4f) {
			const Vec3  v = (pos - g_lastPos) * (1.0f / a_dt);
			const float speed = LengthXZ(v);
			if (speed > 2.5f && speed < 20.0f) {
				const Vec3  f = Rotate(game::Orientation(a_player), { 0, 0, 1 });
				const float diff = std::abs(WrapPi(Heading(v) - Heading(f)));
				if (diff < 0.6f) {
					g_erForward.Add(true, "ER character forward (+Z)");
				} else if (diff > std::numbers::pi_v<float> - 0.6f) {
					g_erForward.Add(false, "ER character forward (+Z)");
				}
			}
		}
		g_lastPos = pos;
		g_havePos = true;

		auto* cam = game::PersCam1();
		if (!cam) {
			return;
		}
		const Vec3  camPos = Row(cam, 3);
		const Vec3  toPlayer = pos + Vec3{ 0, 1.4f, 0 } - camPos;
		const float dist = Length(toPlayer);
		// Not on the first seconds in the world: after a load Elden Ring's camera is still where the title or the last
		// area left it for a while (it logged "156 m from the player" on every start, a false alarm; still 2 s in, live).
		static int s_camFrames = 0;
		if (!g_loggedCam && ++s_camFrames >= 600) {
			g_loggedCam = true;
			const float fov = *reinterpret_cast<const float*>(cam + game::kCamFov);
			Log::Info("calibration: ER camera {} m from the player, fov {:.3f} (radians expected), near {:.3f}, far {:.1f}, cam vtable {}", dist, fov,
				*reinterpret_cast<const float*>(cam + game::kCamNear), *reinterpret_cast<const float*>(cam + game::kCamFar), game::RttiName(cam));
			if (dist > 50.0f) {
				Log::Warn("calibration: camera far from the physics position: are CSCam and physics in the same space?");
			}
		}
		if (dist > 0.8f && dist < 15.0f) {
			const float d = Dot(Normalize(Row(cam, 2)), toPlayer * (1.0f / dist));
			if (d > 0.7f) {
				g_camRow.Add(true, "camera +0x30 row is forward");
			} else if (d < -0.7f) {
				g_camRow.Add(false, "camera +0x30 row is forward");
			}
		}
		const float h = Dot(Row(cam, 0), Cross(Row(cam, 1), Row(cam, 2)));
		if (std::abs(h) > 0.5f) {
			g_handedness = h > 0 ? 1.0f : -1.0f;
		}
	}

	float ErForward()
	{
		if (Cfg().erForward) {
			return static_cast<float>(Cfg().erForward);
		}
		return g_erForward.locked ? static_cast<float>(g_erForward.locked) : 1.0f;
	}

	float CamForwardRow()
	{
		if (Cfg().camForwardRow) {
			return static_cast<float>(Cfg().camForwardRow);
		}
		return g_camRow.locked ? static_cast<float>(g_camRow.locked) : 1.0f;
	}

	float CamHandedness() { return g_handedness != 0 ? g_handedness : 1.0f; }

	bool Locked() { return (Cfg().erForward || g_erForward.locked) && (Cfg().camForwardRow || g_camRow.locked); }

	std::string Describe()
	{
		return std::format("erForward {:+.0f} ({}: {}/{}), camRow {:+.0f} ({}: {}/{}), camHandedness {:+.0f}", ErForward(),
			Cfg().erForward ? "ini" : (g_erForward.locked ? "learned" : "default"), g_erForward.plus, g_erForward.minus, CamForwardRow(),
			Cfg().camForwardRow ? "ini" : (g_camRow.locked ? "learned" : "default"), g_camRow.plus, g_camRow.minus, CamHandedness());
	}
}
