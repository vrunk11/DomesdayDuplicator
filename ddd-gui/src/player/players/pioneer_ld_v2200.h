/************************************************************************

    pioneer_ld_v2200.h

    Pioneer LD-V2200
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#pragma once

#include "pioneer_level_iii.h"
#include "player_definition.h"

namespace ddd::player::pioneer {

// The LD-V2200's CLV time code has no frame field. It reports HMMSS over the
// serial link and accepts the same five digits after TM for a time-code seek;
// the rest of the Level III command set is inherited. This narrow observation
// does not replace the full bench checklist, so bench_verified stays false.
inline constexpr PlayerDefinition kLdV2200 = [] {
  PlayerDefinition definition = LevelIII();
  definition.name = "Pioneer LD-V2200";
  definition.id_code = "07";
  definition.time_code_format = TimeCodeFormat::kHMMSS;
  definition.user_code_error_policy = UserCodeErrorPolicy::kUnsupported;
  definition.commands[Index(PlayerCommand::kSeekTimeCode)] =
      CommandWithHMMSSArgument("TM", "SE", TimeoutClass::kLong);
  return definition;
}();

}  // namespace ddd::player::pioneer
