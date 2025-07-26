#include "tolk.h"
#include <ast.h>
#include <compiler-state.h>

namespace tolk {

void insert_debug_info_inner(SrcLocation loc, ASTNodeKind kind, CodeBlob& code) {
  if (!G.settings.with_debug_info) {
    return;
  }

  if (kind == ast_artificial_aux_vertex || kind == ast_throw_statement) {
    return;
  }

  if (code.prev_ops_kind == Op::_DebugInfo) {
    // std::cerr << "skip repeated debug info" << std::endl;
    // return;
  }

  auto& op = code.emplace_back(loc, Op::_DebugInfo);
  op.debug_idx = G.debug_infos.size();

  auto info = DebugInfo{};
  info.idx = op.debug_idx;
  info.is_entry = kind == ast_function_declaration;

  if (const SrcFile* src_file = loc.get_src_file(); src_file != nullptr) {
    const auto& pos = src_file->convert_offset(loc.get_char_offset());

    info.loc_file = src_file->realpath;
    info.loc_line = pos.line_no;
    info.loc_pos = pos.char_no;
    info.loc_len = pos.line_str.length();
  }

  info.func_name = code.name;
  G.debug_infos.push_back(info);
}

void insert_debug_info(AnyV v, CodeBlob& code) {
  insert_debug_info_inner(v->loc, v->kind, code);
}

}
