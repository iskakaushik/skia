/*
 * Copyright 2016 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#define SK_OPTS_NS skslc_standalone
#include "src/opts/SkChecksum_opts.h"

#include "src/sksl/SkSLCompiler.h"
#include "src/sksl/SkSLDehydrator.h"
#include "src/sksl/SkSLFileOutputStream.h"
#include "src/sksl/SkSLIRGenerator.h"
#include "src/sksl/SkSLStringStream.h"
#include "src/sksl/SkSLUtil.h"
#include "src/sksl/ir/SkSLEnum.h"
#include "src/sksl/ir/SkSLUnresolvedFunction.h"

#include <fstream>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>

void SkDebugf(const char format[], ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

namespace SkOpts {
    decltype(hash_fn) hash_fn = skslc_standalone::hash_fn;
}

enum class ResultCode {
    kSuccess = 0,
    kCompileError = 1,
    kInputError = 2,
    kOutputError = 3,
};

// Given the path to a file (e.g. src/gpu/effects/GrFooFragmentProcessor.fp) and the expected
// filename prefix and suffix (e.g. "Gr" and ".fp"), returns the "base name" of the
// file (in this case, 'FooFragmentProcessor'). If no match, returns the empty string.
static SkSL::String base_name(const SkSL::String& fpPath, const char* prefix, const char* suffix) {
    SkSL::String result;
    const char* end = &*fpPath.end();
    const char* fileName = end;
    // back up until we find a slash
    while (fileName != fpPath && '/' != *(fileName - 1) && '\\' != *(fileName - 1)) {
        --fileName;
    }
    if (!strncmp(fileName, prefix, strlen(prefix)) &&
        !strncmp(end - strlen(suffix), suffix, strlen(suffix))) {
        result.append(fileName + strlen(prefix), end - fileName - strlen(prefix) - strlen(suffix));
    }
    return result;
}

// Given a string containing an SkSL program, searches for a #pragma settings comment, like so:
//    /*#pragma settings Default Sharpen*/
// The passed-in Settings object will be updated accordingly. Any number of options can be provided.
static bool detect_shader_settings(const SkSL::String& text,
                                   SkSL::Program::Settings* settings,
                                   const SkSL::ShaderCapsClass** caps) {
    using Factory = SkSL::ShaderCapsFactory;

    // Find a matching comment and isolate the name portion.
    static constexpr char kPragmaSettings[] = "/*#pragma settings ";
    const char* settingsPtr = strstr(text.c_str(), kPragmaSettings);
    if (settingsPtr != nullptr) {
        // Subtract one here in order to preserve the leading space, which is necessary to allow
        // consumeSuffix to find the first item.
        settingsPtr += strlen(kPragmaSettings) - 1;

        const char* settingsEnd = strstr(settingsPtr, "*/");
        if (settingsEnd != nullptr) {
            SkSL::String settingsText{settingsPtr, size_t(settingsEnd - settingsPtr)};

            // Apply settings as requested. Since they can come in any order, repeat until we've
            // consumed them all.
            for (;;) {
                const size_t startingLength = settingsText.length();

                if (settingsText.consumeSuffix(" AddAndTrueToLoopCondition")) {
                    static auto s_addAndTrueCaps = Factory::AddAndTrueToLoopCondition();
                    *caps = s_addAndTrueCaps.get();
                }
                if (settingsText.consumeSuffix(" BlendModesFailRandomlyForAllZeroVec")) {
                    static auto s_blendZeroCaps = Factory::BlendModesFailRandomlyForAllZeroVec();
                    *caps = s_blendZeroCaps.get();
                }
                if (settingsText.consumeSuffix(" CannotUseFractForNegativeValues")) {
                    static auto s_negativeFractCaps = Factory::CannotUseFractForNegativeValues();
                    *caps = s_negativeFractCaps.get();
                }
                if (settingsText.consumeSuffix(" CannotUseFragCoord")) {
                    static auto s_noFragCoordCaps = Factory::CannotUseFragCoord();
                    *caps = s_noFragCoordCaps.get();
                }
                if (settingsText.consumeSuffix(" CannotUseMinAndAbsTogether")) {
                    static auto s_minAbsCaps = Factory::CannotUseMinAndAbsTogether();
                    *caps = s_minAbsCaps.get();
                }
                if (settingsText.consumeSuffix(" Default")) {
                    static auto s_defaultCaps = Factory::Default();
                    *caps = s_defaultCaps.get();
                }
                if (settingsText.consumeSuffix(" EmulateAbsIntFunction")) {
                    static auto s_emulateAbsIntCaps = Factory::EmulateAbsIntFunction();
                    *caps = s_emulateAbsIntCaps.get();
                }
                if (settingsText.consumeSuffix(" FragCoordsOld")) {
                    static auto s_fragCoordsOld = Factory::FragCoordsOld();
                    *caps = s_fragCoordsOld.get();
                }
                if (settingsText.consumeSuffix(" FragCoordsNew")) {
                    static auto s_fragCoordsNew = Factory::FragCoordsNew();
                    *caps = s_fragCoordsNew.get();
                }
                if (settingsText.consumeSuffix(" GeometryShaderExtensionString")) {
                    static auto s_geometryExtCaps = Factory::GeometryShaderExtensionString();
                    *caps = s_geometryExtCaps.get();
                }
                if (settingsText.consumeSuffix(" GeometryShaderSupport")) {
                    static auto s_geometryShaderCaps = Factory::GeometryShaderSupport();
                    *caps = s_geometryShaderCaps.get();
                }
                if (settingsText.consumeSuffix(" GSInvocationsExtensionString")) {
                    static auto s_gsInvocationCaps = Factory::GSInvocationsExtensionString();
                    *caps = s_gsInvocationCaps.get();
                }
                if (settingsText.consumeSuffix(" IncompleteShortIntPrecision")) {
                    static auto s_incompleteShortIntCaps = Factory::IncompleteShortIntPrecision();
                    *caps = s_incompleteShortIntCaps.get();
                }
                if (settingsText.consumeSuffix(" MustGuardDivisionEvenAfterExplicitZeroCheck")) {
                    static auto s_div0Caps = Factory::MustGuardDivisionEvenAfterExplicitZeroCheck();
                    *caps = s_div0Caps.get();
                }
                if (settingsText.consumeSuffix(" MustForceNegatedAtanParamToFloat")) {
                    static auto s_negativeAtanCaps = Factory::MustForceNegatedAtanParamToFloat();
                    *caps = s_negativeAtanCaps.get();
                }
                if (settingsText.consumeSuffix(" NoGSInvocationsSupport")) {
                    static auto s_noGSInvocations = Factory::NoGSInvocationsSupport();
                    *caps = s_noGSInvocations.get();
                }
                if (settingsText.consumeSuffix(" RemovePowWithConstantExponent")) {
                    static auto s_powCaps = Factory::RemovePowWithConstantExponent();
                    *caps = s_powCaps.get();
                }
                if (settingsText.consumeSuffix(" RewriteDoWhileLoops")) {
                    static auto s_rewriteLoopCaps = Factory::RewriteDoWhileLoops();
                    *caps = s_rewriteLoopCaps.get();
                }
                if (settingsText.consumeSuffix(" ShaderDerivativeExtensionString")) {
                    static auto s_derivativeCaps = Factory::ShaderDerivativeExtensionString();
                    *caps = s_derivativeCaps.get();
                }
                if (settingsText.consumeSuffix(" UnfoldShortCircuitAsTernary")) {
                    static auto s_ternaryCaps = Factory::UnfoldShortCircuitAsTernary();
                    *caps = s_ternaryCaps.get();
                }
                if (settingsText.consumeSuffix(" UsesPrecisionModifiers")) {
                    static auto s_precisionCaps = Factory::UsesPrecisionModifiers();
                    *caps = s_precisionCaps.get();
                }
                if (settingsText.consumeSuffix(" Version110")) {
                    static auto s_version110Caps = Factory::Version110();
                    *caps = s_version110Caps.get();
                }
                if (settingsText.consumeSuffix(" Version450Core")) {
                    static auto s_version450CoreCaps = Factory::Version450Core();
                    *caps = s_version450CoreCaps.get();
                }
                if (settingsText.consumeSuffix(" FlipY")) {
                    settings->fFlipY = true;
                }
                if (settingsText.consumeSuffix(" ForceHighPrecision")) {
                    settings->fForceHighPrecision = true;
                }
                if (settingsText.consumeSuffix(" NoInline")) {
                    settings->fInlineThreshold = 0;
                }
                if (settingsText.consumeSuffix(" InlineThresholdMax")) {
                    settings->fInlineThreshold = INT_MAX;
                }
                if (settingsText.consumeSuffix(" Sharpen")) {
                    settings->fSharpenTextures = true;
                }

                if (settingsText.empty()) {
                    break;
                }
                if (settingsText.length() == startingLength) {
                    printf("Unrecognized #pragma settings: %s\n", settingsText.c_str());
                    return false;
                }
            }
        }
    }

    return true;
}

/**
 * Displays a usage banner; used when the command line arguments don't make sense.
 */
static void show_usage() {
    printf("usage: skslc <input> <output> <flags> -- <input2> <output2> <flags> -- ...\n"
           "\n"
           "Allowed flags:\n"
           "--settings:   honor embedded /*#pragma settings*/ comments.\n"
           "--nosettings: ignore /*#pragma settings*/ comments\n");
}

/**
 * Handle a single input.
 */
ResultCode processCommand(std::vector<SkSL::String>& args) {
    bool honorSettings = true;
    if (args.size() == 4) {
        // Handle four-argument case: `skslc in.sksl out.glsl --settings`
        const SkSL::String& settingsArg = args[3];
        if (settingsArg == "--settings") {
            honorSettings = true;
        } else if (settingsArg == "--nosettings") {
            honorSettings = false;
        } else {
            printf("unrecognized flag: %s\n\n", settingsArg.c_str());
            show_usage();
            return ResultCode::kInputError;
        }
    } else if (args.size() != 3) {
        show_usage();
        return ResultCode::kInputError;
    }

    SkSL::Program::Kind kind;
    const SkSL::String& inputPath = args[1];
    if (inputPath.endsWith(".vert")) {
        kind = SkSL::Program::kVertex_Kind;
    } else if (inputPath.endsWith(".frag") || inputPath.endsWith(".sksl")) {
        kind = SkSL::Program::kFragment_Kind;
    } else if (inputPath.endsWith(".geom")) {
        kind = SkSL::Program::kGeometry_Kind;
    } else if (inputPath.endsWith(".fp")) {
        kind = SkSL::Program::kFragmentProcessor_Kind;
    } else if (inputPath.endsWith(".stage")) {
        kind = SkSL::Program::kPipelineStage_Kind;
    } else {
        printf("input filename must end in '.vert', '.frag', '.geom', '.fp', '.stage', or "
               "'.sksl'\n");
        return ResultCode::kInputError;
    }

    std::ifstream in(inputPath);
    SkSL::String text((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
    if (in.rdstate()) {
        printf("error reading '%s'\n", inputPath.c_str());
        return ResultCode::kInputError;
    }

    SkSL::Program::Settings settings;
    const SkSL::ShaderCapsClass* caps = &SkSL::standaloneCaps;
    if (honorSettings) {
        if (!detect_shader_settings(text, &settings, &caps)) {
            return ResultCode::kInputError;
        }
    }

    const SkSL::String& outputPath = args[2];
    auto emitCompileError = [&](SkSL::FileOutputStream& out, const char* errorText) {
        // Overwrite the compiler output, if any, with an error message.
        out.close();
        SkSL::FileOutputStream errorStream(outputPath);
        errorStream.writeText("### Compilation failed:\n\n");
        errorStream.writeText(errorText);
        errorStream.close();
        // Also emit the error directly to stdout.
        puts(errorText);
    };

    if (outputPath.endsWith(".spirv")) {
        SkSL::FileOutputStream out(outputPath);
        SkSL::Compiler compiler(caps);
        if (!out.isValid()) {
            printf("error writing '%s'\n", outputPath.c_str());
            return ResultCode::kOutputError;
        }
        std::unique_ptr<SkSL::Program> program = compiler.convertProgram(kind, text, settings);
        if (!program || !compiler.toSPIRV(*program, out)) {
            emitCompileError(out, compiler.errorText().c_str());
            return ResultCode::kCompileError;
        }
        if (!out.close()) {
            printf("error writing '%s'\n", outputPath.c_str());
            return ResultCode::kOutputError;
        }
    } else if (outputPath.endsWith(".glsl")) {
        SkSL::FileOutputStream out(outputPath);
        SkSL::Compiler compiler(caps);
        if (!out.isValid()) {
            printf("error writing '%s'\n", outputPath.c_str());
            return ResultCode::kOutputError;
        }
        std::unique_ptr<SkSL::Program> program = compiler.convertProgram(kind, text, settings);
        if (!program || !compiler.toGLSL(*program, out)) {
            emitCompileError(out, compiler.errorText().c_str());
            return ResultCode::kCompileError;
        }
        if (!out.close()) {
            printf("error writing '%s'\n", outputPath.c_str());
            return ResultCode::kOutputError;
        }
    } else if (outputPath.endsWith(".metal")) {
        SkSL::FileOutputStream out(outputPath);
        SkSL::Compiler compiler(caps);
        if (!out.isValid()) {
            printf("error writing '%s'\n", outputPath.c_str());
            return ResultCode::kOutputError;
        }
        std::unique_ptr<SkSL::Program> program = compiler.convertProgram(kind, text, settings);
        if (!program || !compiler.toMetal(*program, out)) {
            emitCompileError(out, compiler.errorText().c_str());
            return ResultCode::kCompileError;
        }
        if (!out.close()) {
            printf("error writing '%s'\n", outputPath.c_str());
            return ResultCode::kOutputError;
        }
    } else if (outputPath.endsWith(".h")) {
        SkSL::FileOutputStream out(outputPath);
        SkSL::Compiler compiler(caps, SkSL::Compiler::kPermitInvalidStaticTests_Flag);
        if (!out.isValid()) {
            printf("error writing '%s'\n", outputPath.c_str());
            return ResultCode::kOutputError;
        }
        settings.fReplaceSettings = false;
        std::unique_ptr<SkSL::Program> program = compiler.convertProgram(kind, text, settings);
        if (!program || !compiler.toH(*program, base_name(inputPath.c_str(), "Gr", ".fp"), out)) {
            emitCompileError(out, compiler.errorText().c_str());
            return ResultCode::kCompileError;
        }
        if (!out.close()) {
            printf("error writing '%s'\n", outputPath.c_str());
            return ResultCode::kOutputError;
        }
    } else if (outputPath.endsWith(".cpp")) {
        SkSL::FileOutputStream out(outputPath);
        SkSL::Compiler compiler(caps, SkSL::Compiler::kPermitInvalidStaticTests_Flag);
        if (!out.isValid()) {
            printf("error writing '%s'\n", outputPath.c_str());
            return ResultCode::kOutputError;
        }
        settings.fReplaceSettings = false;
        std::unique_ptr<SkSL::Program> program = compiler.convertProgram(kind, text, settings);
        if (!program || !compiler.toCPP(*program, base_name(inputPath.c_str(), "Gr", ".fp"), out)) {
            emitCompileError(out, compiler.errorText().c_str());
            return ResultCode::kCompileError;
        }
        if (!out.close()) {
            printf("error writing '%s'\n", outputPath.c_str());
            return ResultCode::kOutputError;
        }
    } else if (outputPath.endsWith(".dehydrated.sksl")) {
        SkSL::FileOutputStream out(outputPath);
        SkSL::Compiler compiler(caps);
        if (!out.isValid()) {
            printf("error writing '%s'\n", outputPath.c_str());
            return ResultCode::kOutputError;
        }
        auto [symbols, elements] = compiler.loadModule(
                kind, SkSL::Compiler::MakeModulePath(inputPath.c_str()), nullptr);
        SkSL::Dehydrator dehydrator;
        dehydrator.write(*symbols);
        dehydrator.write(elements);
        SkSL::String baseName = base_name(inputPath, "", ".sksl");
        SkSL::StringStream buffer;
        dehydrator.finish(buffer);
        const SkSL::String& data = buffer.str();
        out.printf("static uint8_t SKSL_INCLUDE_%s[] = {", baseName.c_str());
        for (size_t i = 0; i < data.length(); ++i) {
            out.printf("%s%d,", dehydrator.prefixAtOffset(i), uint8_t(data[i]));
        }
        out.printf("};\n");
        out.printf("static constexpr size_t SKSL_INCLUDE_%s_LENGTH = sizeof(SKSL_INCLUDE_%s);\n",
                   baseName.c_str(), baseName.c_str());
        if (!out.close()) {
            printf("error writing '%s'\n", outputPath.c_str());
            return ResultCode::kOutputError;
        }
    } else {
        printf("expected output filename to end with '.spirv', '.glsl', '.cpp', '.h', or '.metal'");
        return ResultCode::kInputError;  // the "output filename" is still an input argument
    }
    return ResultCode::kSuccess;
}

int main(int argc, const char** argv) {
    // Search the command line for -- delimiters. When a -- is reached, we process one command.
    std::vector<SkSL::String> args = {argv[0]};
    auto resultCode = ResultCode::kSuccess;
    for (int index = 1; index < argc; ++index) {
        SkSL::String arg = argv[index];
        if (arg != "--") {
            // We found an argument. Remember it.
            args.push_back(std::move(arg));
        } else {
            // We found a delimiter. If we have any arguments stored up, process them as a command.
            if (args.size() > 1) {
                ResultCode outcome = processCommand(args);
                resultCode = std::max(resultCode, outcome);

                // Clear every argument except the first ("skslc").
                args.resize(1);
            }
        }
    }

    // Execute the final command in the batch.
    if (args.size() > 1) {
        ResultCode outcome = processCommand(args);
        resultCode = std::max(resultCode, outcome);
    }

    // Return the "worst" status we encountered. For our purposes, compilation errors are the least
    // serious, because they are expected to occur in unit tests. Other types of errors are not
    // expected at all during a build.
    return (int) resultCode;
}
