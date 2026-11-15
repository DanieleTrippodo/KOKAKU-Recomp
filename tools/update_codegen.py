with open('tools/PSPRecomp/tools/codegen_main.cpp', 'r', encoding='utf-8') as f:
    code = f.read()

# 1. Update emit_target signature and implementation
old_emit_target = '''void emit_target(std::ostringstream &body, std::uint32_t target,
                 const std::set<std::uint32_t> &labels, const char *indent,
                 std::uint32_t executable_base = 0u,
                 std::uint32_t unit_span_bytes = 0u,
                 const std::map<std::uint32_t, std::uint16_t> *direct_entry_ids = nullptr,
                 const std::set<std::uint32_t> *import_stubs = nullptr) {
    if (labels.contains(target)) {
        body << indent << "goto L_" << psprecomp::hex32(target).substr(2) << ";\\n";
        return;
    }

    // A fixed J/JAL to a PSP import must return to the outer dispatcher.
    // Trying the generated-unit chain first is guaranteed to fail because import
    // registration deliberately poisons/replaces that exact PC, and these stubs
    // are frequently hot in real titles. Emit the minimal correct handoff directly.
    if (import_stubs != nullptr && import_stubs->contains(target)) {
        body << indent << "ctx.pc = " << psprecomp::hex32(target) << "u; return;\\n";
        return;
    }

    // A fixed-span AOT partition can split one guest CFG across neighbouring
    // C++ translation units. knows the destination bucket for every
    // fixed direct edge, so the common case bypasses Runtime's large per-PC
    // chain table and indexes the tiny unit table instead. A bucket containing
    // an import/HLE/host replacement is marked overridden at registration time
    // and invoke_chained_unit() falls back to the exact per-PC lookup there.
    if (unit_span_bytes != 0u && target >= executable_base) {
        const std::uint32_t unit = (target - executable_base) / unit_span_bytes;
        body << indent << "(void)" << direct_unit_chain_expression(unit, target, direct_entry_ids)
             << "; return;\\n";
    } else {
        body << indent << "ctx.pc = " << psprecomp::hex32(target)
             << "u; (void)rt.invoke_chained_call(ctx, &aot_mem); return;\\n";
    }
}'''

new_emit_target = '''void emit_target(std::ostringstream &body, std::uint32_t target,
                 const std::set<std::uint32_t> &labels, const char *indent,
                 std::uint32_t executable_base = 0u,
                 std::uint32_t unit_span_bytes = 0u,
                 const std::map<std::uint32_t, std::uint16_t> *direct_entry_ids = nullptr,
                 const std::set<std::uint32_t> *import_stubs = nullptr,
                 const std::set<std::uint32_t> *valid_buckets = nullptr) {
    if (labels.contains(target)) {
        body << indent << "goto L_" << psprecomp::hex32(target).substr(2) << ";\\n";
        return;
    }

    if (import_stubs != nullptr && import_stubs->contains(target)) {
        body << indent << "ctx.pc = " << psprecomp::hex32(target) << "u; return;\\n";
        return;
    }

    if (unit_span_bytes != 0u && target >= executable_base) {
        const std::uint32_t unit = (target - executable_base) / unit_span_bytes;
        if (valid_buckets != nullptr && valid_buckets->contains(unit)) {
            body << indent << "(void)" << direct_unit_chain_expression(unit, target, direct_entry_ids)
                 << "; return;\\n";
            return;
        }
    }
    body << indent << "ctx.pc = " << psprecomp::hex32(target)
         << "u; (void)rt.invoke_chained_call(ctx, &aot_mem); return;\\n";
}'''

assert old_emit_target in code, 'old_emit_target not found'
code = code.replace(old_emit_target, new_emit_target)

# 2. Update GeneratedFunctionInput
old_gfi = '''struct GeneratedFunctionInput {
    std::string name;
    std::uint32_t address{};
    std::set<std::uint32_t> instructions;
    std::set<std::uint32_t> entry_labels;
    std::uint32_t executable_base{};
    std::uint32_t unit_span_bytes{};
    const std::map<std::uint32_t, std::uint16_t> *direct_entry_ids{};
    const std::set<std::uint32_t> *import_stubs{};
};'''

new_gfi = '''struct GeneratedFunctionInput {
    std::string name;
    std::uint32_t address{};
    std::set<std::uint32_t> instructions;
    std::set<std::uint32_t> entry_labels;
    std::uint32_t executable_base{};
    std::uint32_t unit_span_bytes{};
    const std::map<std::uint32_t, std::uint16_t> *direct_entry_ids{};
    const std::set<std::uint32_t> *import_stubs{};
    const std::set<std::uint32_t> *valid_buckets{};
};'''

assert old_gfi in code, 'old_gfi not found'
code = code.replace(old_gfi, new_gfi)

# 3. Update emit_target calls in emit_function_source
code = code.replace('function.import_stubs);', 'function.import_stubs, function.valid_buckets);')

# 4. Update direct_unit check in emit_function_source
old_direct = '''                        const bool direct_unit = function.unit_span_bytes != 0u &&
                            target >= function.executable_base;'''

new_direct = '''                        const bool direct_unit = function.unit_span_bytes != 0u &&
                            target >= function.executable_base &&
                            function.valid_buckets != nullptr &&
                            function.valid_buckets->contains((target - function.executable_base) / function.unit_span_bytes);'''

assert old_direct in code, 'old_direct not found'
code = code.replace(old_direct, new_direct)

# 5. In generate_auto, collect valid_buckets and pass to GeneratedFunctionInput
old_seed = '''    std::map<std::uint32_t, std::uint16_t> direct_entry_ids;
    for (const auto &unit : units) {
        std::uint16_t id = 1u;
        for (const auto label : unit.entries) direct_entry_ids[label] = id++;
    }'''

new_seed = '''    std::map<std::uint32_t, std::uint16_t> direct_entry_ids;
    std::set<std::uint32_t> valid_buckets;
    for (const auto &unit : units) {
        valid_buckets.insert(unit.bucket);
        std::uint16_t id = 1u;
        for (const auto label : unit.entries) direct_entry_ids[label] = id++;
    }'''

assert old_seed in code, 'old_seed not found'
code = code.replace(old_seed, new_seed)

old_gfi_call = '''        const GeneratedFunctionInput generated_unit{
            "recomp_unit_" + suffix_text,
            executable_base + unit.bucket * unit_span_bytes,
            unit.instructions,
            unit.entries,
            executable_base,
            unit_span_bytes,
            &direct_entry_ids,
            &import_stubs,
        };'''

new_gfi_call = '''        const GeneratedFunctionInput generated_unit{
            "recomp_unit_" + suffix_text,
            executable_base + unit.bucket * unit_span_bytes,
            unit.instructions,
            unit.entries,
            executable_base,
            unit_span_bytes,
            &direct_entry_ids,
            &import_stubs,
            &valid_buckets,
        };'''

assert old_gfi_call in code, 'old_gfi_call not found'
code = code.replace(old_gfi_call, new_gfi_call)

with open('tools/PSPRecomp/tools/codegen_main.cpp', 'w', encoding='utf-8') as f:
    f.write(code)

print('Updated codegen_main.cpp successfully!')
