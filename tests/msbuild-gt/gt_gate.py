# -*- coding: utf-8 -*-
"""MSBuild 金标六维对账门禁。

由评测期的四份原型脚本（全库对账 / 数字复核 / 失配归类 / 金标
普查）产品化合并而来：把入库语料（仓内中性夹具的工程求值金标
groundtruth.json 与 ccg 记录通道产物 compile_commands.json +
pch.json，见 corpus/ 目录与 README.md 的语料来源说明）做六维
对账，全部实测指标与入库基线逐值比对，任何一维漂移即失败。

六维口径（判据源自金标评测战役的定案判据；每维对应基线 JSON 的同名节）：
  tu_coverage（TU 覆盖）—— 金标构建参与实例集与 cdb 条目集双向
     对账，条目数差额必须恰为 .ixx 双趟数；
  defines（宏定义）—— 保序逐位（含重复出现位置）；
  include（包含目录）—— best-match 保序口径 + 存在性终验口径
     双计，共享文件的多条目歧义由存在性口径消解；
  pch（预编译头）—— 金标 Create/Use TU 与 pch.json
     producers/consumers 双向集合对账，外加 pch.json 内嵌 cdb
     指纹的交叉验证；
  toolset（工具集）—— 金标 VCToolsVersion 与 cdb
     -fms-compatibility-version 的 14.x/19.x 换算全等；
  misc_invariants（杂项不变量）—— 代表 TU 的深挖不变量（语言
     标准映射、禁用警告集合、附加选项在场、共享文件 defines
     序等），代表 TU 由语料内容特征程序化挑选，不依赖具体文件名；
  module_switches（模块开关）—— /ifcOutput 等六项带值开关在 cdb
     中成对带值（带值开关成对保留修复后的锁定形态：开关名与值
     token 都在场，消费端喂引擎前成对剥除），裸拼写回潮即失败；
     脚本内的 XFAIL 判读机构保留，供其他维度将来按已知缺陷/已
     修复两种形态登记。

退出码协议（与指纹文件配套的漂移分罪）：
  0 —— 全绿（含 XFAIL 维持）；
  1 —— 输入/协议错误（文件缺失、基线与指纹文件失锁、JSON 坏行）；
  3 —— 语料指纹全等但六维数字漂移：ccg 回归嫌疑（数字变好也红，
       须人工重审基线并同批重锁指纹）；
  4 —— 语料或引擎指纹漂移：语料已重采/重生，基线待重审，
       不算 ccg 回归。
"""

import argparse
import hashlib
import json
import os
import sys
from collections import Counter, defaultdict

EXIT_OK = 0
EXIT_INPUT = 1
EXIT_REGRESSION = 3
EXIT_CORPUS_DRIFT = 4

# 语料文件名到指纹文件键名的映射（fingerprints.json 的 corpus 节）。
CORPUS_FILES = ("groundtruth.json", "compile_commands.json", "pch.json")
SHA_KEYS = {
    "groundtruth.json": "groundtruth_sha256",
    "compile_commands.json": "compile_commands_sha256",
    "pch.json": "pch_sha256",
}

# 语言标准 metadata 值到 /std: 旗标的映射（CL task SwitchOrder 直译）。
STD_MAP = {
    "stdcpplatest": "/std:c++latest",
    "stdcpp26": "/std:c++26",
    "stdcpp23": "/std:c++23",
    "stdcpp20": "/std:c++20",
    "stdcpp17": "/std:c++17",
    "stdcpp14": "/std:c++14",
    "Default": None,
    "": None,
}

# 金标 TU 层 diff 存储键到 ClCompile item definition metadata 键的映射。
FIELD_TO_MSB = {
    "defines": "PreprocessorDefinitions",
    "undefines": "UndefinePreprocessorDefinitions",
    "include_dirs": "AdditionalIncludeDirectories",
    "external_include": "ExternalIncludePath",
    "forced_includes": "ForcedIncludeFiles",
    "additional_options": "AdditionalOptions",
    "language_standard": "LanguageStandard",
    "language_standard_c": "LanguageStandard_C",
    "pch_use": "PrecompiledHeader",
    "pch_through": "PrecompiledHeaderFile",
    "pch_file": "PrecompiledHeaderOutputFile",
    "compile_as": "CompileAs",
    "conformance_mode": "ConformanceMode",
    "runtime_library": "RuntimeLibrary",
    "warning_level": "WarningLevel",
    "disable_warnings": "DisableSpecificWarnings",
    "external_warning": "ExternalWarningLevel",
    "multiproc": "MultiProcessorCompilation",
    "debug_format": "DebugInformationFormat",
    "eh": "ExceptionHandling",
    "object_file": "ObjectFileName",
    "program_database": "ProgramDataBaseFileName",
    "module_output": "ModuleOutputFile",
    "scan_modules": "ScanSourceForModuleDependencies",
    "translate_includes": "TranslateIncludes",
    "module_dependencies": "AdditionalModuleDependencies",
    "bmi_dirs": "AdditionalBMIDirectories",
    "treat_wchar_builtin": "TreatWChar_tAsBuiltInType",
    "runtime_typeinfo": "RuntimeTypeInfo",
    "string_pooling": "StringPooling",
    "function_level": "FunctionLevelLinking",
    "intrinsics": "IntrinsicFunctions",
    "minimal_rebuild": "MinimalRebuild",
    "browse_info": "BrowseInformation",
    "disable_lang_ext": "DisableLanguageExtensions",
    "calling_convention": "CallingConvention",
}


def die(code, message):
    """按协议退出码终止并输出原因。"""
    print("gt_gate: " + message, file=sys.stderr)
    sys.exit(code)


def sha256_file(path):
    """按字节流计算文件内容指纹。"""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def norm(path):
    """路径规范化：正斜杠、normpath、小写——对账两侧的统一形态。"""
    return os.path.normpath(str(path).replace("\\", "/")).replace("\\", "/").lower()


def split_list(raw):
    """按 MSBuild 列表分隔符拆分，空段丢弃。"""
    return [x for x in raw.split(";") if x.strip() != ""]


def gold_final(project, tu):
    """重建金标 TU 终值：TU 层 diff 存储缺失的字段回退 item definition。"""
    idef = project["cl_item_definition"]
    values = {}
    for field, msb in FIELD_TO_MSB.items():
        values[field] = tu.get(field, idef.get(msb, ""))
    return values


def project_dir(project):
    """金标工程的 MSBuildProjectDirectory 求值终值。"""
    return project["top_props"].get("MSBuildProjectDirectory", "")


def gold_inc_abs(project, raw):
    """金标 include 目录绝对化：相对路径按工程目录消解（CL task 同款）。"""
    out = []
    for x in split_list(raw):
        if len(x) >= 2 and x[1] == ":":
            out.append(norm(x))
        else:
            out.append(norm(os.path.join(project_dir(project), x)))
    return out


def sandbox_map(dir_norm, project):
    """cdb 侧沙箱 IntDir 映射回金标真实 IntDir（口径差处置规则）。"""
    marker = "/msbuild-sandbox/int"
    if marker in dir_norm:
        intdir = norm(project["top_props"].get("IntDir", ""))
        return intdir if intdir else dir_norm
    return dir_norm


def ccg_parse(args):
    """把一条 ccg clang-cl 命令行拆成语义桶（对账与深挖共用的解析器）。"""
    out = {
        "includes": [], "defines": [], "imsvc": [], "wd": [], "we": [],
        "std": None, "pch_yu": None, "pch_yc": None, "pch_fp": None,
        "interface": False, "internal_partition": False, "export_header": False,
        "ifc_output": None, "scan": False, "srcdep": False, "tc": False, "tp": False,
        "clang_extra": [], "ifc_output_bare": False,
        "scan_bare": False, "srcdep_bare": False,
    }
    i = 0
    n = len(args)
    while i < n:
        a = args[i]
        nxt = args[i + 1] if i + 1 < n else None
        follows_value = nxt is not None and not nxt.startswith(("/", "-"))
        if a == "clang-cl" or a == "--driver-mode=cl" or a == "/c":
            pass
        elif a.startswith("/I"):
            out["includes"].append(a[2:])
        elif a.startswith("/D"):
            out["defines"].append(a[2:])
        elif a.startswith("/wd"):
            out["wd"].append(a[3:])
        elif a.startswith("/we"):
            out["we"].append(a[3:])
        elif a == "-imsvc":
            i += 1
            if i < n:
                out["imsvc"].append(args[i])
        elif a.startswith("/std:"):
            out["std"] = a[5:]
        elif a == "/Yu":
            out["pch_yu"] = ""
        elif a.startswith("/Yu"):
            out["pch_yu"] = a[3:]
        elif a == "/Yc":
            out["pch_yc"] = ""
        elif a.startswith("/Yc"):
            out["pch_yc"] = a[3:]
        elif a.startswith("/Fp"):
            out["pch_fp"] = a[3:] if len(a) > 3 else ""
        elif a == "/interface":
            out["interface"] = True
        elif a == "/internalPartition":
            out["internal_partition"] = True
        elif a == "/exportHeader":
            out["export_header"] = True
        elif a == "/ifcOutput":
            if follows_value:
                out["ifc_output"] = nxt
                i += 1
            else:
                out["ifc_output"] = "<bare>"
                out["ifc_output_bare"] = True
        elif a == "/scanDependencies":
            out["scan"] = True
            out["scan_bare"] = not follows_value
        elif a == "/sourceDependencies":
            out["srcdep"] = True
            out["srcdep_bare"] = not follows_value
        elif a == "/TC":
            out["tc"] = True
        elif a == "/TP":
            out["tp"] = True
        elif a.startswith("-fms-compatibility-version="):
            out["clang_extra"].append(a)
        elif a.startswith("-finput-charset="):
            out["clang_extra"].append(a)
        elif a == "/Zc:twoPhase-":
            out["clang_extra"].append(a)
        i += 1
    return out


def pick_entry(entries, gold_defines):
    """为金标 TU 实例选最可能对应的 cdb 条目。

    cdb 无工程归属标记：.ixx 双趟优先编译趟
    （/sourceDependencies 在场或非扫描趟），多条目共享文件再按
    defines 集合 Jaccard 相似度择优。选择歧义由 include 维度的
    存在性终验口径独立消解。
    """
    def score(entry):
        parsed = ccg_parse(entry["arguments"])
        base = 1.0 if (parsed["srcdep"] or not parsed["scan"]) else 0.0
        if gold_defines is not None:
            g = set(gold_defines)
            c = set(parsed["defines"])
            union = g | c
            base += (len(g & c) / len(union)) * 100 if union else 100.0
        return base
    ranked = sorted(entries, key=score, reverse=True)
    return ranked[0], ccg_parse(ranked[0]["arguments"])


def gold_census(gt):
    """金标普查（只依赖 groundtruth.json 的锚数字）。"""
    ps = gt["projects"]
    ext = Counter()
    for p in ps:
        for t in p.get("tus", []):
            ext[t["file"].lower().rsplit(".", 1)[-1]] += 1
    pch_itemdef = Counter()
    for p in ps:
        d = p.get("cl_item_definition", {})
        pch_itemdef[d.get("PrecompiledHeader", "<absent>")] += 1
    return {
        "projects": len(ps),
        "tu_instances": sum(p.get("tu_count", 0) for p in ps),
        "tu_built": sum(p.get("tu_count", 0) for p in ps if p.get("build_in_solution", True)),
        "tu_not_built": sum(p.get("tu_count", 0) for p in ps if not p.get("build_in_solution", True)),
        "degraded_projects": sum(1 for p in ps if p.get("degraded_missing_imports")),
        "not_built_projects": sum(1 for p in ps if not p.get("build_in_solution", True)),
        "ext_census": dict(sorted(ext.items())),
        "platform_toolset_census": dict(
            sorted(Counter(p["top_props"].get("PlatformToolset", "?") for p in ps).items())),
        "vctools_census": dict(
            sorted(Counter(p["top_props"].get("VCToolsVersion", "") for p in ps).items())),
        "pch_itemdef_census": dict(sorted(pch_itemdef.items())),
        "tu_pch_use_overrides": sum(1 for p in ps for t in p.get("tus", []) if "pch_use" in t),
        "tu_defines_overrides": sum(1 for p in ps for t in p.get("tus", []) if "defines" in t),
        "tu_include_overrides": sum(1 for p in ps for t in p.get("tus", []) if "include_dirs" in t),
        "self_check_checks": gt["meta"]["self_check"]["tu_field_checks"],
        "self_check_mismatches": gt["meta"]["self_check"]["mismatches"],
    }


def dim_tu_coverage(gt, cc, cc_by_file):
    """TU 覆盖维度：金标构建参与集与 cdb 条目集的双向对账。"""
    total = matched = missing = ide_only = ide_only_in_ccg = 0
    for p in gt["projects"]:
        builds = p.get("build_in_solution", True)
        for tu in p.get("tus", []):
            total += 1
            entries = cc_by_file.get(norm(tu["abs"]))
            if not builds:
                # 无 Build.0 行的工程：金标按 IDE 语义保留并单列桶，
                # 其 TU 不出现在 cdb 是正确行为。
                ide_only += 1
                if entries:
                    ide_only_in_ccg += 1
                continue
            if entries:
                matched += 1
            else:
                missing += 1
    gold_keys = {norm(tu["abs"]) for p in gt["projects"] for tu in p.get("tus", [])}
    extra = sum(1 for k in cc_by_file if k not in gold_keys)
    ixx_entries = sum(1 for e in cc if e["file"].lower().endswith(".ixx"))
    ixx_files = sum(1 for k, v in cc_by_file.items()
                    if k.endswith(".ixx") and len(v) == 2)
    return {
        "gold_tu_instances": total,
        "matched_instances": matched,
        "missing_in_ccg": missing,
        "ccg_entries": len(cc),
        "ccg_distinct_files": len(cc_by_file),
        "extra_files_in_ccg": extra,
        "ixx_entries": ixx_entries,
        "ixx_double_pass_files": ixx_files,
        "ide_only_instances": ide_only,
        "ide_only_in_ccg_via_others": ide_only_in_ccg,
    }


def dim_defines(gt, cc_by_file):
    """defines 维度：保序逐位（原始值不修剪，含重复出现位置）。"""
    seq_eq = set_only = checked = 0
    for p in gt["projects"]:
        if not p.get("build_in_solution", True):
            continue
        for tu in p.get("tus", []):
            entries = cc_by_file.get(norm(tu["abs"]))
            if not entries:
                continue
            g = gold_final(p, tu)
            _, cp = pick_entry(entries, split_list(g["defines"]))
            gd = split_list(g["defines"])
            cd = cp["defines"]
            checked += 1
            if gd == cd:
                seq_eq += 1
            elif set(gd) == set(cd):
                set_only += 1
    return {"seq_equal": seq_eq, "set_equal_only": set_only, "checked": checked}


def dim_include(gt, cc_by_file):
    """include 维度：best-match 保序口径 + 存在性终验口径双计。"""
    seq_eq = set_only = checked = 0
    exists_total = exists_some = 0
    for p in gt["projects"]:
        if not p.get("build_in_solution", True):
            continue
        for tu in p.get("tus", []):
            entries = cc_by_file.get(norm(tu["abs"]))
            if not entries:
                continue
            g = gold_final(p, tu)
            _, cp = pick_entry(entries, split_list(g["defines"]))
            gi = gold_inc_abs(p, g["include_dirs"])
            ci = [sandbox_map(norm(x), p) for x in cp["includes"]]
            gi_d = list(dict.fromkeys(gi))
            ci_d = list(dict.fromkeys(ci))
            checked += 1
            if gi_d == ci_d:
                seq_eq += 1
            elif set(gi_d) == set(ci_d):
                set_only += 1
            # 存在性终验：金标序与该文件任一条 cdb 条目精确相等即算命中。
            exists_total += 1
            hit = False
            for e in entries:
                inc = [sandbox_map(norm(a[2:]), p)
                       for a in e["arguments"] if a.startswith("/I")]
                if gi_d == list(dict.fromkeys(inc)):
                    hit = True
                    break
            if hit:
                exists_some += 1
    return {
        "best_match_seq_equal": seq_eq,
        "best_match_set_equal_only": set_only,
        "checked": checked,
        "exists_exact_some": exists_some,
        "exists_none_match": exists_total - exists_some,
    }


def dim_pch(gt, pch, cdb_sha):
    """pch 维度：金标 Create/Use 与 pch.json 旁车双向对账 + cdb 指纹交叉。"""
    gold_create_instances = 0
    gold_create_files = set()
    gold_use = gold_none = 0
    for p in gt["projects"]:
        if not p.get("build_in_solution", True):
            continue
        base_use = p["cl_item_definition"].get("PrecompiledHeader", "")
        for tu in p.get("tus", []):
            use = tu.get("pch_use", base_use)
            if use == "Create":
                gold_create_instances += 1
                gold_create_files.add(norm(tu["abs"]))
            elif use == "Use":
                gold_use += 1
            else:
                gold_none += 1
    producers = pch.get("producers", [])
    consumers = pch.get("consumers", [])
    ccg_files = {norm(pr.get("source", "")) for pr in producers}
    return {
        "gold_create_instances": gold_create_instances,
        "gold_create_unique_files": len(gold_create_files),
        "gold_use_tus": gold_use,
        "gold_not_using_tus": gold_none,
        "ccg_producers": len(producers),
        "ccg_producer_unique_files": len(ccg_files),
        "producer_overlap_files": len(ccg_files & gold_create_files),
        "producer_gold_only": len(gold_create_files - ccg_files),
        "producer_ccg_only": len(ccg_files - gold_create_files),
        "ccg_consumers": len(consumers),
        "ccg_not_using": len(pch.get("not_using", [])),
        "cdb_sha_cross_match": pch.get("cdb_sha256") == cdb_sha,
    }


def dim_toolset(gt, cc_by_file):
    """toolset 维度：VCToolsVersion 与 -fms-compatibility-version 的换算全等。"""
    checked = agree = 0
    for p in gt["projects"]:
        if not p.get("build_in_solution", True) or p.get("tu_count", 0) == 0:
            continue
        vt = p["top_props"].get("VCToolsVersion", "")
        for tu in p.get("tus", []):
            entries = cc_by_file.get(norm(tu["abs"]))
            if entries:
                parsed = ccg_parse(entries[0]["arguments"])
                msv = next((a.split("=")[1] for a in parsed["clang_extra"]
                            if a.startswith("-fms-compatibility-version=")), None)
                if msv:
                    # 金标 VCToolsVersion 为空的工程（求值未解析出
                    # 工具集版本）不参与换算对账，不进计数。
                    if vt:
                        checked += 1
                        if msv.startswith("19." + vt[3:]):
                            agree += 1
                break
    return {"checked": checked, "agree": agree}


def dim_misc_invariants(gt, cc_by_file):
    """杂项不变量维度：代表 TU 深挖不变量（选择器按内容特征，不锁文件名）。"""
    shared_pick = unique_pick = ixx_pick = None
    for p in gt["projects"]:
        if not p.get("build_in_solution", True):
            continue
        for tu in p.get("tus", []):
            entries = cc_by_file.get(norm(tu["abs"]))
            if not entries:
                continue
            g = gold_final(p, tu)
            has_defines = len(split_list(g["defines"])) > 0
            if tu["file"].lower().endswith(".ixx") and ixx_pick is None:
                ixx_pick = (p, tu, entries)
            elif len(entries) > 1 and has_defines and shared_pick is None:
                shared_pick = (p, tu, entries)
            elif len(entries) == 1 and has_defines and unique_pick is None:
                unique_pick = (p, tu, entries)

    result = {
        "shared_pick_found": shared_pick is not None,
        "unique_pick_found": unique_pick is not None,
        "ixx_pick_found": ixx_pick is not None,
    }

    def tu_checks(pick):
        p, tu, entries = pick
        g = gold_final(p, tu)
        entry, cp = pick_entry(entries, split_list(g["defines"]))
        gd = split_list(g["defines"])
        gi_d = list(dict.fromkeys(gold_inc_abs(p, g["include_dirs"])))
        ci_d = list(dict.fromkeys(
            sandbox_map(norm(x), p) for x in cp["includes"]))
        # cdb 侧解析存的是 /std: 去前缀值，比较时拼回完整旗标形态；
        # 金标语言标准为 Default/空时断言 cdb 无 /std: 旗标。
        expected_std = STD_MAP.get(g["language_standard"], "<unmapped>")
        actual_std = ("/std:" + cp["std"]) if cp["std"] is not None else None
        return {
            "defines_order_equal": gd == cp["defines"],
            "include_order_equal": gi_d == ci_d,
            "std_map_ok": actual_std == expected_std,
            "wd_set_equal": sorted(split_list(g["disable_warnings"])) == sorted(cp["wd"]),
            "addopts_all_present": all(
                t in entry["arguments"] for t in g["additional_options"].split()),
        }

    if shared_pick is not None:
        for k, v in tu_checks(shared_pick).items():
            result["shared_" + k] = v
    if unique_pick is not None:
        for k, v in tu_checks(unique_pick).items():
            result["unique_" + k] = v
    if ixx_pick is not None:
        p, tu, entries = ixx_pick
        passes = [ccg_parse(e["arguments"]) for e in entries]
        _, cp = pick_entry(entries, None)
        g = gold_final(p, tu)
        result["ixx_entry_count"] = len(entries)
        result["ixx_scan_pass_present"] = any(x["scan"] for x in passes)
        result["ixx_compile_pass_present"] = any(x["srcdep"] for x in passes)
        # /interface 的来源是 targets 执行期对 CompileAs 的改写（纯求值
        # 金标停在 Default），形态断言只锁"旗标在场"。
        result["ixx_interface_present"] = cp["interface"]
        gd = split_list(g["defines"])
        result["ixx_defines_order_equal"] = gd == cp["defines"]
    return result


def dim_module_switches(cc):
    """模块开关形态维度：.ixx 条目的带值开关值 token 普查（现状口径：
    只数裸拼写回潮，成对带值形态已锁定为绿灯基准）。"""
    ixx = [e for e in cc if e["file"].lower().endswith(".ixx")]
    bare_ifc = bare_scan = bare_srcdep = 0
    for e in ixx:
        parsed = ccg_parse(e["arguments"])
        if parsed["ifc_output_bare"]:
            bare_ifc += 1
        if parsed["scan_bare"]:
            bare_scan += 1
        if parsed["srcdep_bare"]:
            bare_srcdep += 1
    return {
        "ixx_entries": len(ixx),
        "bare_ifc_output": bare_ifc,
        "bare_scan_dependencies": bare_scan,
        "bare_source_dependencies": bare_srcdep,
    }


def dim_shared_files(gt, cc_by_file):
    """共享文件口径差量化：cdb 多条目共享文件与金标多工程文件计数。"""
    cdb_multi = {k: len(v) for k, v in cc_by_file.items() if len(v) > 1}
    gold_by = Counter()
    for p in gt["projects"]:
        for tu in p.get("tus", []):
            gold_by[tu["abs"].lower()] += 1
    gold_multi = {k: v for k, v in gold_by.items() if v > 1}
    return {
        "cdb_multi_entry_files": len(cdb_multi),
        "cdb_multi_entry_max": max(cdb_multi.values()) if cdb_multi else 0,
        "gold_multi_project_files": len(gold_multi),
        "gold_multi_project_max": max(gold_multi.values()) if gold_multi else 0,
    }


def diff_metrics(actual, expected, prefix=""):
    """递归展开实测与期望的数值/布尔差异清单（漂移分罪的明细面）。"""
    out = []
    for key in sorted(set(actual) | set(expected)):
        a = actual.get(key, "<absent>")
        e = expected.get(key, "<absent>")
        full = prefix + key
        if isinstance(a, dict) and isinstance(e, dict):
            out.extend(diff_metrics(a, e, full + "."))
        elif a != e:
            out.append("%s: expected %r, actual %r" % (full, e, a))
    return out


def main():
    parser = argparse.ArgumentParser(
        description="MSBuild 金标六维对账门禁（基线锁定 + 双指纹分罪）")
    parser.add_argument("--corpus", required=True,
                        help="语料目录（groundtruth.json / compile_commands.json / pch.json 三件套）")
    parser.add_argument("--baseline", required=True, help="六维基线 JSON")
    parser.add_argument("--fingerprints", required=True, help="双指纹 JSON")
    parser.add_argument("--report", help="实测指标 JSON 落盘路径（可选）")
    args = parser.parse_args()

    # 指纹文件先验：基线文件的内容指纹必须与登记值一致——基线数字
    # 被修改而未同批重锁指纹即在此响亮失败（防基线静默篡改）。
    try:
        with open(args.fingerprints, encoding="utf-8") as f:
            prints = json.load(f)
    except (OSError, ValueError) as ex:
        die(EXIT_INPUT, "fingerprints unreadable: %s" % ex)
    baseline_sha = sha256_file(args.baseline)
    if baseline_sha != prints.get("baseline_sha256"):
        die(EXIT_INPUT,
            "baseline/fingerprints relock mismatch: baseline sha256 %s != "
            "registered %s — baseline was modified; relock fingerprints "
            "explicitly and rerun (see tests/msbuild-gt/README.md)"
            % (baseline_sha, prints.get("baseline_sha256")))

    paths = {name: os.path.join(args.corpus, name) for name in CORPUS_FILES}
    missing = [n for n, p in paths.items() if not os.path.isfile(p)]
    if missing:
        die(EXIT_INPUT, "corpus files missing: %s" % ", ".join(missing))

    # 语料与引擎指纹比对：漂移即退出 4（语料重生，基线待重审）。
    corpus_sha = {name: sha256_file(paths[name]) for name in CORPUS_FILES}
    registered = prints.get("corpus", {})
    drift = [n for n in CORPUS_FILES
             if registered.get(SHA_KEYS[n]) != corpus_sha[n]]
    if drift:
        print("gt_gate: corpus fingerprint drift: %s" % ", ".join(drift),
              file=sys.stderr)
        for n in drift:
            print("  %s: registered %s, actual %s"
                  % (n, registered.get(SHA_KEYS[n]), corpus_sha[n]),
                  file=sys.stderr)
        die(EXIT_CORPUS_DRIFT,
            "corpus fingerprint drift (exit 4): corpus was recaptured or "
            "regenerated; the six-dim baseline needs manual review, then "
            "relock fingerprints per tests/msbuild-gt/README.md")

    try:
        with open(paths["groundtruth.json"], encoding="utf-8") as f:
            gt = json.load(f)
        with open(paths["compile_commands.json"], encoding="utf-8") as f:
            cc = json.load(f)
        with open(paths["pch.json"], encoding="utf-8") as f:
            pch = json.load(f)
        with open(args.baseline, encoding="utf-8") as f:
            baseline = json.load(f)
    except (OSError, ValueError) as ex:
        die(EXIT_INPUT, "json unreadable: %s" % ex)

    # 引擎指纹：金标 meta 的引擎版本（换引擎重生金标即漂移）。
    meta = gt.get("meta", {})
    engine = prints.get("engine", {})
    engine_drift = []
    if engine.get("gold_engine_version") != meta.get("engine_version"):
        engine_drift.append("engine_version: registered %r, actual %r"
                            % (engine.get("gold_engine_version"),
                               meta.get("engine_version")))
    if engine_drift:
        print("gt_gate: engine fingerprint drift:", file=sys.stderr)
        print("  " + "\n  ".join(engine_drift), file=sys.stderr)
        die(EXIT_CORPUS_DRIFT,
            "engine fingerprint drift (exit 4): the gold corpus was "
            "regenerated with a different engine; review the baseline "
            "and relock fingerprints")

    cc_by_file = defaultdict(list)
    for e in cc:
        cc_by_file[norm(e["file"])].append(e)

    metrics = {
        "gold_census": gold_census(gt),
        "tu_coverage": dim_tu_coverage(gt, cc, cc_by_file),
        "defines": dim_defines(gt, cc_by_file),
        "include": dim_include(gt, cc_by_file),
        "pch": dim_pch(gt, pch, corpus_sha["compile_commands.json"]),
        "toolset": dim_toolset(gt, cc_by_file),
        "misc_invariants": dim_misc_invariants(gt, cc_by_file),
        "module_switches": dim_module_switches(cc),
        "shared_files": dim_shared_files(gt, cc_by_file),
    }
    # .ixx 双趟的对账公式：cdb 条目总数减金标匹配实例数必须恰为
    # 双趟文件数（每文件第二趟一余条），多一条少一条都是 TU 覆盖
    # 维度的形态破坏。
    metrics["tu_coverage"]["entries_minus_matched"] = \
        metrics["tu_coverage"]["ccg_entries"] - metrics["tu_coverage"]["matched_instances"]

    if args.report:
        with open(args.report, "w", encoding="utf-8") as f:
            json.dump(metrics, f, ensure_ascii=False, indent=1, sort_keys=True)

    # 基线比对：普通维度全等；XFAIL 维度按缺陷/修复两种已知形态判读。
    regressions = []
    for section, expected in sorted(baseline.items()):
        if section == "schema" or not isinstance(expected, dict):
            continue
        actual = metrics.get(section)
        if actual is None:
            regressions.append("%s: section missing in metrics" % section)
            continue
        if expected.get("xfail"):
            defect = expected["expect_defect"]
            fixed = expected.get("expect_fixed", {})
            if actual == defect:
                print("gt_gate: %s: XFAIL holds (known defect form present, "
                      "see baseline note)" % section)
                continue
            matches_fixed = all(actual.get(k) == v for k, v in fixed.items())
            if matches_fixed:
                regressions.append(
                    "%s: XFAIL dimension reached the FIXED form (actual %r) — "
                    "if the fix batch has landed, this is the designed "
                    "flip-red prompt: set baseline xfail to false, update "
                    "expected values from the measurement, and relock "
                    "fingerprints in the same change" % (section, actual))
            else:
                regressions.append(
                    "%s: XFAIL dimension deviates from both known forms "
                    "(defect %r, actual %r)" % (section, defect, actual))
            continue
        expected_values = {k: v for k, v in expected.items()
                           if k not in ("xfail", "defect", "note", "expect_defect",
                                        "expect_fixed")}
        regressions.extend(diff_metrics(actual, expected_values, section + "."))

    if regressions:
        print("gt_gate: six-dim baseline mismatch (exit 3; better-looking "
              "numbers fail too):", file=sys.stderr)
        for r in regressions:
            print("  " + r, file=sys.stderr)
        die(EXIT_REGRESSION,
            "baseline drift: %d place(s) require manual review" % len(regressions))

    print("gt_gate: six dims all green")
    print("  census: %d projects / %d TU instances (built %d + not-built %d) "
          "/ self-check %d fields 0 mismatch"
          % (metrics["gold_census"]["projects"],
             metrics["gold_census"]["tu_instances"],
             metrics["gold_census"]["tu_built"],
             metrics["gold_census"]["tu_not_built"],
             metrics["gold_census"]["self_check_checks"]))
    print("  tu-coverage %d/%d matched (cdb %d entries = matched + %d ixx second pass)"
          % (metrics["tu_coverage"]["matched_instances"],
             metrics["tu_coverage"]["matched_instances"],
             metrics["tu_coverage"]["ccg_entries"],
             metrics["tu_coverage"]["entries_minus_matched"]))
    print("  defines %d/%d | include exists %d/%d (best-match seq %d + set-only %d)"
          % (metrics["defines"]["seq_equal"], metrics["defines"]["checked"],
             metrics["include"]["exists_exact_some"],
             metrics["include"]["checked"],
             metrics["include"]["best_match_seq_equal"],
             metrics["include"]["best_match_set_equal_only"]))
    print("  pch %d producers vs gold %d Create / %d consumers vs gold %d Use "
          "/ cdb sha cross-match %s"
          % (metrics["pch"]["ccg_producers"],
             metrics["pch"]["gold_create_instances"],
             metrics["pch"]["ccg_consumers"],
             metrics["pch"]["gold_use_tus"],
             metrics["pch"]["cdb_sha_cross_match"]))
    print("  toolset %d/%d | module switches: %d ixx entries, "
          "%d bare /ifcOutput (paired-value form locked)"
          % (metrics["toolset"]["agree"], metrics["toolset"]["checked"],
             metrics["module_switches"]["ixx_entries"],
             metrics["module_switches"]["bare_ifc_output"]))


if __name__ == "__main__":
    main()
