load(
    "//devtools/python/blaze:pytype.bzl",
    "pytype_strict_binary",
    "pytype_strict_contrib_test",
    "pytype_strict_library",
)
load("//third_party/bazel_rules/rules_cc/cc:cc_binary.bzl", "cc_binary")
load("//third_party/bazel_rules/rules_cc/cc:cc_library.bzl", "cc_library")
load("//third_party/bazel_rules/rules_cc/cc:cc_test.bzl", "cc_test")

package(
    default_visibility = ["//experimental/users/joseluiszapata:__subpackages__"],
)

licenses(["notice"])

cc_library(
    name = "openfx_headers",
    hdrs = [
        "ofx/openfx_include/ofxCore.h",
        "ofx/openfx_include/ofxImageEffect.h",
        "ofx/openfx_include/ofxInteract.h",
        "ofx/openfx_include/ofxMemory.h",
        "ofx/openfx_include/ofxMessage.h",
        "ofx/openfx_include/ofxMultiThread.h",
        "ofx/openfx_include/ofxParam.h",
        "ofx/openfx_include/ofxPixels.h",
        "ofx/openfx_include/ofxProperty.h",
    ],
)

cc_library(
    name = "papercraft_kernel",
    srcs = ["ofx/papercraft_kernel.cc"],
    hdrs = ["ofx/papercraft_kernel.h"],
    copts = ["-fvisibility=hidden"],
)

cc_binary(
    name = "libpapercraft_studio_pro.so",
    srcs = ["ofx/papercraft_ofx_plugin.cc"],
    additional_linker_inputs = ["ofx/openfx_sym.lds"],
    copts = ["-fvisibility=hidden"],
    linkopts = ["-Wl,--version-script=$(location ofx/openfx_sym.lds)"],
    linkshared = 1,
    linkstatic = 1,
    deps = [
        ":openfx_headers",
        ":papercraft_kernel",
    ],
)

cc_library(
    name = "papercraft_standalone_compile_check",
    srcs = [
        "ofx/papercraft_kernel.cc",
        "ofx/papercraft_ofx_plugin.cc",
    ],
    hdrs = ["ofx/papercraft_kernel.h"],
    copts = [
        "-fvisibility=hidden",
        "-Iexperimental/users/joseluiszapata/davinci_resolve_plugins/papercraft_pro/ofx",
        "-Iexperimental/users/joseluiszapata/davinci_resolve_plugins/papercraft_pro/ofx/openfx_include",
    ],
    defines = ["PAPERCRAFT_STANDALONE_OFX"],
    deps = [":openfx_headers"],
)

genrule(
    name = "linux_ofx_bundle_binary",
    srcs = [":libpapercraft_studio_pro.so"],
    outs = ["PaperCraftStudioPro.ofx"],
    cmd = "cp $< $@ && chmod 0755 $@",
)

cc_test(
    name = "papercraft_ofx_test",
    srcs = ["ofx/papercraft_ofx_test.cc"],
    data = [":linux_ofx_bundle_binary"],
    linkopts = ["-ldl"],
    deps = [
        ":openfx_headers",
        "//testing/base/public:gunit_main",
    ],
)

filegroup(
    name = "ofx_package_sources",
    srcs = [
        "ofx/Info.plist",
        "ofx/Install_PaperCraft_OpenFX_macOS.command",
        "ofx/install_linux_ofx.sh",
        "ofx/openfx_include/LICENSE",
        "ofx/openfx_include/ofxCore.h",
        "ofx/openfx_include/ofxImageEffect.h",
        "ofx/openfx_include/ofxInteract.h",
        "ofx/openfx_include/ofxMemory.h",
        "ofx/openfx_include/ofxMessage.h",
        "ofx/openfx_include/ofxMultiThread.h",
        "ofx/openfx_include/ofxParam.h",
        "ofx/openfx_include/ofxPixels.h",
        "ofx/openfx_include/ofxProperty.h",
        "ofx/openfx_sym.lds",
        "ofx/papercraft_kernel.cc",
        "ofx/papercraft_kernel.h",
        "ofx/papercraft_ofx_plugin.cc",
    ],
)

filegroup(
    name = "docs_files",
    srcs = [
        "README.en.md",
        "README.md",
        "docs/design_doc.md",
        "docs/developer_guide.md",
        "docs/user_guide.md",
    ],
)

filegroup(
    name = "repo_source_files",
    srcs = [
        "BUILD",
        "build_papercraft_bundle.py",
        "bundle_builder_lib.py",
        "drfx_validator.py",
        "fusion_templates.py",
        "ofx/papercraft_ofx_test.cc",
        "papercraft_pro_test.py",
        "png_renderer.py",
        "sfx_synthesizer.py",
        ":docs_files",
        ":ofx_package_sources",
    ],
)

pytype_strict_library(
    name = "png_renderer",
    srcs = ["png_renderer.py"],
)

pytype_strict_library(
    name = "fusion_templates",
    srcs = ["fusion_templates.py"],
)

pytype_strict_library(
    name = "sfx_synthesizer",
    srcs = ["sfx_synthesizer.py"],
)

pytype_strict_library(
    name = "drfx_validator",
    srcs = ["drfx_validator.py"],
    deps = [":png_renderer"],
)

pytype_strict_library(
    name = "bundle_builder_lib",
    srcs = ["bundle_builder_lib.py"],
    data = [
        ":linux_ofx_bundle_binary",
        ":repo_source_files",
    ],
    deps = [
        ":drfx_validator",
        ":fusion_templates",
        ":png_renderer",
        ":sfx_synthesizer",
    ],
)

pytype_strict_binary(
    name = "build_papercraft_bundle",
    srcs = ["build_papercraft_bundle.py"],
    deps = [
        ":bundle_builder_lib",
        "//third_party/py/absl:app",
        "//third_party/py/absl/flags",
        "//third_party/py/absl/logging",
    ],
)

pytype_strict_contrib_test(
    name = "papercraft_pro_test",
    srcs = ["papercraft_pro_test.py"],
    deps = [
        ":bundle_builder_lib",
        ":drfx_validator",
        ":fusion_templates",
        ":png_renderer",
        ":sfx_synthesizer",
        "//third_party/py/absl/testing:absltest",
    ],
)

genrule(
    name = "papercraft_release_bundle",
    srcs = [
        "README.md",
        ":linux_ofx_bundle_binary",
        ":repo_source_files",
    ],
    outs = [
        "PaperCraft_Studio_Pro.drfx",
        "PaperCraft_Studio_Pro_Full_Bundle.zip",
        "PaperCraft_Studio_Pro_Complete_Repo.zip",
    ],
    cmd = (
        "TMP_OUT=$$(mktemp -d) && " +
        "$(location :build_papercraft_bundle) " +
        "--output_dir=$$TMP_OUT " +
        "--readme_path=$(location README.md) " +
        "--linux_ofx_path=$(location :linux_ofx_bundle_binary) && " +
        "cp $$TMP_OUT/PaperCraft_Studio_Pro.drfx " +
        "$(location PaperCraft_Studio_Pro.drfx) && " +
        "cp $$TMP_OUT/PaperCraft_Studio_Pro_Full_Bundle.zip " +
        "$(location PaperCraft_Studio_Pro_Full_Bundle.zip) && " +
        "cp $$TMP_OUT/PaperCraft_Studio_Pro_Complete_Repo.zip " +
        "$(location PaperCraft_Studio_Pro_Complete_Repo.zip) && " +
        "rm -rf $$TMP_OUT"
    ),
    tools = [":build_papercraft_bundle"],
)
