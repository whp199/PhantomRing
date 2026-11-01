#pragma once

// [Mantle]'s tunables for er/Mantle.h PlanMantle (its own header: er/Config.h holds them without er/Kinematic.h's names).

namespace phantom::er
{
	struct MantleParams
	{
		float minHeight{ 0.5f };       // a ledge's top this high above the feet at least (lower: a step, walked onto) ...
		float maxHeight{ 1.8f };       // ... and at most (higher: a wall)
		float reach{ 0.6f };           // its face at most this far from the body (m beyond the radius)
		float vaultMaxHeight{ 1.35f };  // a thin obstacle up to this high with ground behind it is vaulted over (Limgrave's wooden rail
		                                // fences: their top rail 1.26 m over the ground, live 2026-10-31 -- 1.2 refused them) ...
		float vaultMaxDepth{ 0.7f };   // ... when its top is at most this deep (a fence, a low wall, a railing)
		float vaultMaxDrop{ 1.6f };    // ... and the ground behind it at most this far below the feet (more: a drop, no vault)
		float maxSlope{ 0.6f };        // the top's rise over run toward the landing (0.6 ~ 31 degrees)
		float climbRoom{ 1.0f };       // m of room above the ledge's top over where he rises (his body bent over it)
		float climbBaseS{ 1.8f };      // climb up: base + per metre of height (MGSV's full climb-up onto a crate / drum: ~2 s from its Y,
		float climbPerM{ 0.2f };       // whatever the height; live 2026-10-31: 1988 ms)
		// vault: base + per metre of height. MGSV's own vault, from the request (live 2026-10-31, both games: the guest stages Snake
		// and presses Y ~0.2 s later): on top at ~0.8 s, down on the far side at ~1.35 s; 0.55 (0.68 s for 0.5 m) had Elden
		// Ring's path over and done while MGSV's pose was still reaching for the top.
		float vaultBaseS{ 1.1f };
		float vaultPerM{ 0.25f };
	};
}
