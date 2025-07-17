#include "tolk.h"
#include "pipeline.h"
#include "compiler-state.h"
#include "type-system.h"
#include "td/utils/JsonBuilder.h"
#include <fstream>

namespace tolk {

void pipeline_process_debug_info(std::ostream& debug_out) {
  if (!G.settings.with_debug_info) {
    return;
  }


  td::JsonBuilder _jb;
  auto objb = _jb.enter_object();

  {
    td::JsonBuilder jsonb;
    auto arrb = jsonb.enter_array();
    for (auto glob_var : G.all_global_vars) {
      auto vb = arrb.enter_value();
      auto ob = vb.enter_object();

      ob("name", glob_var->name);
      ob("type", glob_var->declared_type->as_human_readable());
    }
    arrb.leave();

    objb("globals", td::JsonRaw(jsonb.string_builder().as_cslice()));
  }

  {
    td::JsonBuilder jsonb;
    auto arrb = jsonb.enter_array();
    for (auto di : G.debug_infos) {
      auto vb = arrb.enter_value();
      auto ob = vb.enter_object();
      ob("file", di.loc_file);
      ob("line", (td::int64)di.loc_line);
      ob("pos", (td::int64)di.loc_pos);
      ob("length", (td::int64)di.loc_len);

      td::JsonBuilder varb;
      auto vararrb = varb.enter_array();
      for (auto var_and_value : di.vars) {
        const auto [var, value] = var_and_value;
        auto varb2 = vararrb.enter_value();
        auto varbo = varb2.enter_object();
        varbo("name", var.name.empty() ? "'" + std::to_string(var.ir_idx) : var.name);
        varbo("type", var.v_type == nullptr ? "" : var.v_type->as_human_readable());
        if (!value.empty()) {
          varbo("value", value);
        }
      }
      vararrb.leave();

      td::JsonRaw vararrs(varb.string_builder().as_cslice());

      ob("vars", vararrs);
      ob("func", di.func_name);
    }
    arrb.leave();

    objb("locations", td::JsonRaw(jsonb.string_builder().as_cslice()));
  }

  objb.leave();

  debug_out << _jb.string_builder().as_cslice().str();
}

}  // namespace tolk
