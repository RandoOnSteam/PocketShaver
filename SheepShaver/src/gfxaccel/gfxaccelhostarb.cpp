#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "gfxaccelhost.h"

static std::string GfxAccelHostArbTrim(const std::string &text)
{
	size_t first;
	size_t last;
	first = text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
		return std::string();
	last = text.find_last_not_of(" \t\r\n");
	return text.substr(first, last - first + 1);
}

static std::vector<std::string> GfxAccelHostArbSplit(const std::string &text, char separator)
{
	std::vector<std::string> pieces;
	size_t start;
	size_t found;
	start = 0;
	found = text.find(separator);
	while (found != std::string::npos) {
		pieces.push_back(GfxAccelHostArbTrim(text.substr(start, found - start)));
		start = found + 1;
		found = text.find(separator, start);
	}
	pieces.push_back(GfxAccelHostArbTrim(text.substr(start)));
	return pieces;
}

static int GfxAccelHostArbIndex(const std::string &text, const char *prefix, int *index)
{
	size_t length;
	length = strlen(prefix);
	if (text.compare(0, length, prefix) != 0 || text.size() <= length + 1 || text[text.size() - 1] != ']')
		return 0;
	*index = atoi(text.c_str() + length);
	return 1;
}

static std::string GfxAccelHostArbNumber(int value)
{
	char digits[16];
	snprintf(digits, sizeof(digits), "%d", value);
	return std::string(digits);
}

static int GfxAccelHostArbBinding(const std::string &text, std::string *expression)
{
	int index;
	if (GfxAccelHostArbIndex(text, "program.local[", &index) || GfxAccelHostArbIndex(text, "program.env[", &index)) {
		*expression = "Local[" + GfxAccelHostArbNumber(index) + "]";
		return 1;
	}
	if (GfxAccelHostArbIndex(text, "fragment.texcoord[", &index) || GfxAccelHostArbIndex(text, "vertex.texcoord[", &index)) {
		*expression = "input.tex" + GfxAccelHostArbNumber(index);
		return 1;
	}
	if (text == "fragment.texcoord" || text == "vertex.texcoord") {
		*expression = "input.tex0";
		return 1;
	}
	if (text == "fragment.color" || text == "fragment.color.primary" || text == "vertex.color") {
		*expression = "input.color";
		return 1;
	}
	if (text == "vertex.position") {
		*expression = "input.position";
		return 1;
	}
	if (text == "fragment.position") {
		*expression = "input.position";
		return 1;
	}
	return 0;
}

static int GfxAccelHostArbSource(const std::string &raw, std::string *expression)
{
	std::string text;
	std::string prefix;
	std::string name;
	std::string swizzle;
	std::string binding;
	size_t dot;
	text = GfxAccelHostArbTrim(raw);
	if (!text.empty() && text[0] == '-') {
		prefix = "-";
		text = GfxAccelHostArbTrim(text.substr(1));
	}
	name = text;
	dot = text.rfind('.');
	if (dot != std::string::npos && text.find(']', dot) == std::string::npos) {
		swizzle = text.substr(dot + 1);
		if (swizzle.find_first_not_of("xyzwrgba") == std::string::npos && swizzle.size() <= 4) {
			name = text.substr(0, dot);
		} else {
			swizzle.clear();
		}
	}
	if (GfxAccelHostArbBinding(name, &binding))
		name = binding;
	if (swizzle.size() == 1)
		swizzle = swizzle + swizzle + swizzle + swizzle;
	if (swizzle.empty())
		*expression = prefix + name;
	else
		*expression = prefix + name + "." + swizzle;
	return 1;
}

static std::string GfxAccelHostArbSwizzleComponent(const std::string &component, const std::string &source)
{
	std::string text;
	std::string sign;
	text = component;
	if (!text.empty() && (text[0] == '-' || text[0] == '+')) {
		if (text[0] == '-')
			sign = "-";
		text = text.substr(1);
	}
	if (text == "0" || text == "1")
		return sign + text + ".0";
	return sign + source + "." + text;
}

static int GfxAccelHostArbInstruction(const std::string &statement, int fragmentprogram, std::string *body)
{
	std::vector<std::string> arguments;
	std::vector<std::string> sources;
	std::string opcode;
	std::string destination;
	std::string mask;
	std::string value;
	std::string a;
	std::string b;
	std::string c;
	size_t space;
	size_t dot;
	size_t index;
	int saturate;
	space = statement.find_first_of(" \t");
	if (space == std::string::npos)
		return 0;
	opcode = statement.substr(0, space);
	arguments = GfxAccelHostArbSplit(statement.substr(space + 1), ',');
	saturate = 0;
	if (opcode.size() > 4 && opcode.compare(opcode.size() - 4, 4, "_SAT") == 0) {
		saturate = 1;
		opcode = opcode.substr(0, opcode.size() - 4);
	}
	if (opcode == "KIL") {
		if (!GfxAccelHostArbSource(arguments[0], &a))
			return 0;
		*body += "\tclip(" + a + ");\n";
		return 1;
	}
	destination = arguments[0];
	dot = destination.rfind('.');
	if (dot != std::string::npos && destination.find(']', dot) == std::string::npos && destination.find_first_not_of("xyzw", dot + 1) == std::string::npos) {
		mask = destination.substr(dot);
		destination = destination.substr(0, dot);
	}
	if (destination == "result.color")
		destination = "outputcolor";
	if (destination.compare(0, 7, "result.") == 0 || destination.compare(0, 9, "fragment.") == 0)
		return 0;
	if (opcode == "SWZ") {
		if (arguments.size() != 6 || !GfxAccelHostArbSource(arguments[1], &a))
			return 0;
		value = "float4(" + GfxAccelHostArbSwizzleComponent(arguments[2], a) + ", " + GfxAccelHostArbSwizzleComponent(arguments[3], a) + ", " + GfxAccelHostArbSwizzleComponent(arguments[4], a) + ", " + GfxAccelHostArbSwizzleComponent(arguments[5], a) + ")";
	} else if (opcode == "TEX" || opcode == "TXP" || opcode == "TXB") {
		std::string sampler;
		int unit;
		if (arguments.size() != 4 || !GfxAccelHostArbSource(arguments[1], &a))
			return 0;
		if (!GfxAccelHostArbIndex(arguments[2], "texture[", &unit))
			return 0;
		sampler = "SampleTexture2D";
		if (arguments[3] == "RECT")
			sampler = "SampleTextureRectangle";
		else if (arguments[3] != "2D")
			return 0;
		value = sampler + "(" + GfxAccelHostArbNumber(unit) + ", " + a + ")";
	} else {
		for (index = 1; index < arguments.size(); index++) {
			if (!GfxAccelHostArbSource(arguments[index], &a))
				return 0;
			sources.push_back(a);
		}
		if (sources.empty())
			return 0;
		a = sources[0];
		if (sources.size() > 1)
			b = sources[1];
		if (sources.size() > 2)
			c = sources[2];
		if (opcode == "MOV")
			value = a;
		else if (opcode == "ADD")
			value = "(" + a + " + " + b + ")";
		else if (opcode == "SUB")
			value = "(" + a + " - " + b + ")";
		else if (opcode == "MUL")
			value = "(" + a + " * " + b + ")";
		else if (opcode == "MAD")
			value = "(" + a + " * " + b + " + " + c + ")";
		else if (opcode == "MAX")
			value = "max(" + a + ", " + b + ")";
		else if (opcode == "MIN")
			value = "min(" + a + ", " + b + ")";
		else if (opcode == "LRP")
			value = "lerp(" + c + ", " + b + ", " + a + ")";
		else if (opcode == "FRC")
			value = "frac(" + a + ")";
		else if (opcode == "FLR")
			value = "floor(" + a + ")";
		else if (opcode == "ABS")
			value = "abs(" + a + ")";
		else if (opcode == "SGE")
			value = "step(" + b + ", " + a + ")";
		else if (opcode == "SLT")
			value = "(float4(1.0, 1.0, 1.0, 1.0) - step(" + b + ", " + a + "))";
		else if (opcode == "CMP")
			value = "lerp(" + b + ", " + c + ", step(float4(0.0, 0.0, 0.0, 0.0), " + a + "))";
		else if (opcode == "DP3")
			value = "Replicate(dot((" + a + ").xyz, (" + b + ").xyz))";
		else if (opcode == "DP4")
			value = "Replicate(dot(" + a + ", " + b + "))";
		else if (opcode == "DPH")
			value = "Replicate(dot(float4((" + a + ").xyz, 1.0), " + b + "))";
		else if (opcode == "POW")
			value = "Replicate(pow((" + a + ").x, (" + b + ").x))";
		else if (opcode == "RCP")
			value = "Replicate(1.0 / (" + a + ").x)";
		else if (opcode == "RSQ")
			value = "Replicate(1.0 / sqrt(abs((" + a + ").x)))";
		else if (opcode == "LG2")
			value = "Replicate(log2(abs((" + a + ").x)))";
		else if (opcode == "EX2")
			value = "Replicate(pow(2.0, (" + a + ").x))";
		else
			return 0;
	}
	(void)fragmentprogram;
	if (saturate)
		value = "saturate(" + value + ")";
	if (mask.empty())
		*body += "\t" + destination + " = " + value + ";\n";
	else
		*body += "\t" + destination + mask + " = (" + value + ")" + mask + ";\n";
	return 1;
}

static int GfxAccelHostArbDeclaration(const std::string &statement, int fragmentprogram, std::string *declarations, std::string *results)
{
	std::vector<std::string> names;
	std::vector<std::string> values;
	std::string name;
	std::string value;
	std::string expression;
	size_t equals;
	size_t index;
	int unit;
	if (statement.compare(0, 5, "TEMP ") == 0) {
		names = GfxAccelHostArbSplit(statement.substr(5), ',');
		for (index = 0; index < names.size(); index++)
			*declarations += "\tfloat4 " + names[index] + " = float4(0.0, 0.0, 0.0, 0.0);\n";
		return 1;
	}
	equals = statement.find('=');
	if (equals == std::string::npos)
		return 0;
	name = GfxAccelHostArbTrim(statement.substr(statement.find_first_of(" \t"), equals - statement.find_first_of(" \t")));
	value = GfxAccelHostArbTrim(statement.substr(equals + 1));
	if (statement.compare(0, 6, "PARAM ") == 0) {
		if (!value.empty() && value[0] == '{') {
			if (value[value.size() - 1] != '}')
				return 0;
			values = GfxAccelHostArbSplit(value.substr(1, value.size() - 2), ',');
			while (values.size() < 4) {
				if (values.size() == 3)
					values.push_back("1.0");
				else
					values.push_back("0.0");
			}
			*declarations += "\tfloat4 " + name + " = float4(" + values[0] + ", " + values[1] + ", " + values[2] + ", " + values[3] + ");\n";
			return 1;
		}
		if (!GfxAccelHostArbBinding(value, &expression))
			return 0;
		*declarations += "\tfloat4 " + name + " = " + expression + ";\n";
		return 1;
	}
	if (statement.compare(0, 7, "ATTRIB ") == 0) {
		if (!GfxAccelHostArbBinding(value, &expression))
			return 0;
		*declarations += "\tfloat4 " + name + " = " + expression + ";\n";
		return 1;
	}
	if (statement.compare(0, 7, "OUTPUT ") == 0) {
		*declarations += "\tfloat4 " + name + " = float4(0.0, 0.0, 0.0, 0.0);\n";
		if (value == "result.color") {
			*results += "\toutputcolor = " + name + ";\n";
			return 1;
		}
		if (fragmentprogram)
			return 0;
		if (GfxAccelHostArbIndex(value, "result.texcoord[", &unit)) {
			*results += "\toutput.tex" + GfxAccelHostArbNumber(unit) + " = " + name + ";\n";
			return 1;
		}
		if (value == "result.texcoord") {
			*results += "\toutput.tex0 = " + name + ";\n";
			return 1;
		}
		if (value == "result.position") {
			*results += "\toutput.position = float4(" + name + ".x, " + name + ".y, (" + name + ".z + " + name + ".w) * 0.5, " + name + ".w);\n";
			return 1;
		}
		return 0;
	}
	return 0;
}

static const char GfxAccelHostArbFragmentPrelude[] =
	"cbuffer ProgramData : register(b0, space3) { float4 Local[96]; float4 Size[8]; };\n"
	"[[vk::combinedImageSampler]] Texture2D Image0 : register(t0, space2); [[vk::combinedImageSampler]] SamplerState Filter0 : register(s0, space2);\n"
	"[[vk::combinedImageSampler]] Texture2D Image1 : register(t1, space2); [[vk::combinedImageSampler]] SamplerState Filter1 : register(s1, space2);\n"
	"[[vk::combinedImageSampler]] Texture2D Image2 : register(t2, space2); [[vk::combinedImageSampler]] SamplerState Filter2 : register(s2, space2);\n"
	"[[vk::combinedImageSampler]] Texture2D Image3 : register(t3, space2); [[vk::combinedImageSampler]] SamplerState Filter3 : register(s3, space2);\n"
	"[[vk::combinedImageSampler]] Texture2D Image4 : register(t4, space2); [[vk::combinedImageSampler]] SamplerState Filter4 : register(s4, space2);\n"
	"[[vk::combinedImageSampler]] Texture2D Image5 : register(t5, space2); [[vk::combinedImageSampler]] SamplerState Filter5 : register(s5, space2);\n"
	"[[vk::combinedImageSampler]] Texture2D Image6 : register(t6, space2); [[vk::combinedImageSampler]] SamplerState Filter6 : register(s6, space2);\n"
	"[[vk::combinedImageSampler]] Texture2D Image7 : register(t7, space2); [[vk::combinedImageSampler]] SamplerState Filter7 : register(s7, space2);\n"
	"#define SampleTextureRectangle(unit, coordinates) Image##unit.Sample(Filter##unit, (coordinates).xy / Size[unit].xy)\n"
	"#define SampleTexture2D(unit, coordinates) Image##unit.Sample(Filter##unit, (coordinates).xy)\n"
	"float4 Replicate(float value) { return float4(value, value, value, value); }\n"
	"struct ProgramVarying { float4 position : SV_Position; float4 color : TEXCOORD0; float4 tex0 : TEXCOORD1; float4 tex1 : TEXCOORD2; float4 tex2 : TEXCOORD3; float4 tex3 : TEXCOORD4; };\n"
	"float4 PSMain(ProgramVarying input) : SV_Target0\n{\n\tfloat4 outputcolor = float4(0.0, 0.0, 0.0, 1.0);\n";

static const char GfxAccelHostArbFragmentPreludeMetal[] =
	"#include <metal_stdlib>\nusing namespace metal;\n"
	"#define lerp mix\n#define frac fract\n#define clip(value) if (any((value) < 0.0)) discard_fragment()\n"
	"#define Local programdata.Local\n#define Size programdata.Size\n"
	"#define SampleTextureRectangle(unit, coordinates) Image##unit.sample(Filter##unit, (coordinates).xy / Size[unit].xy)\n"
	"#define SampleTexture2D(unit, coordinates) Image##unit.sample(Filter##unit, (coordinates).xy)\n"
	"struct ProgramData { float4 Local[96]; float4 Size[8]; };\n"
	"float4 Replicate(float value) { return float4(value, value, value, value); }\n"
	"struct ProgramVarying { float4 position [[position]]; float4 color; float4 tex0; float4 tex1; float4 tex2; float4 tex3; };\n"
	"fragment float4 PSMain(ProgramVarying input [[stage_in]], constant ProgramData &programdata [[buffer(0)]], "
	"texture2d<float> Image0 [[texture(0)]], sampler Filter0 [[sampler(0)]], texture2d<float> Image1 [[texture(1)]], sampler Filter1 [[sampler(1)]], "
	"texture2d<float> Image2 [[texture(2)]], sampler Filter2 [[sampler(2)]], texture2d<float> Image3 [[texture(3)]], sampler Filter3 [[sampler(3)]], "
	"texture2d<float> Image4 [[texture(4)]], sampler Filter4 [[sampler(4)]], texture2d<float> Image5 [[texture(5)]], sampler Filter5 [[sampler(5)]], "
	"texture2d<float> Image6 [[texture(6)]], sampler Filter6 [[sampler(6)]], texture2d<float> Image7 [[texture(7)]], sampler Filter7 [[sampler(7)]])\n"
	"{\n\tfloat4 outputcolor = float4(0.0, 0.0, 0.0, 1.0);\n";

static const char GfxAccelHostArbVertexPreludeMetal[] =
	"#include <metal_stdlib>\nusing namespace metal;\n"
	"#define lerp mix\n#define frac fract\n#define Local programdata.Local\n"
	"struct VertexData { float4 Local[96]; };\n"
	"float4 Replicate(float value) { return float4(value, value, value, value); }\n"
	"struct ProgramAttributes { float4 position [[attribute(0)]]; float4 color [[attribute(1)]]; float4 secondary [[attribute(2)]]; float4 tex0 [[attribute(3)]]; float4 tex1 [[attribute(4)]]; float fogCoord [[attribute(5)]]; float4 eye [[attribute(6)]]; };\n"
	"struct ProgramVarying { float4 position [[position]]; float4 color; float4 tex0; float4 tex1; float4 tex2; float4 tex3; };\n"
	"vertex ProgramVarying VSMain(ProgramAttributes input [[stage_in]], constant VertexData &programdata [[buffer(0)]])\n{\n"
	"\tProgramVarying output;\n\tfloat4 outputcolor = float4(0.0, 0.0, 0.0, 0.0);\n"
	"\toutput.position = input.position;\n\toutput.color = input.color;\n\toutput.tex0 = input.tex0;\n\toutput.tex1 = input.tex1;\n"
	"\toutput.tex2 = float4(0.0, 0.0, 0.0, 1.0);\n\toutput.tex3 = float4(0.0, 0.0, 0.0, 1.0);\n";

static const char GfxAccelHostArbVertexPrelude[] =
	"cbuffer ProgramData : register(b0, space1) { float4 Local[96]; };\n"
	"float4 Replicate(float value) { return float4(value, value, value, value); }\n"
	"struct ProgramAttributes { float4 position : TEXCOORD0; float4 color : TEXCOORD1; float4 secondary : TEXCOORD2; float4 tex0 : TEXCOORD3; float4 tex1 : TEXCOORD4; float fogCoord : TEXCOORD5; float4 eye : TEXCOORD6; };\n"
	"struct ProgramVarying { float4 position : SV_Position; float4 color : TEXCOORD0; float4 tex0 : TEXCOORD1; float4 tex1 : TEXCOORD2; float4 tex2 : TEXCOORD3; float4 tex3 : TEXCOORD4; };\n"
	"ProgramVarying VSMain(ProgramAttributes input)\n{\n"
	"\tProgramVarying output;\n\tfloat4 outputcolor = float4(0.0, 0.0, 0.0, 0.0);\n"
	"\toutput.position = input.position;\n\toutput.color = input.color;\n\toutput.tex0 = input.tex0;\n\toutput.tex1 = input.tex1;\n"
	"\toutput.tex2 = float4(0.0, 0.0, 0.0, 1.0);\n\toutput.tex3 = float4(0.0, 0.0, 0.0, 1.0);\n";

char *GfxAccelHostTranslateProgram(const char *programtext, uint32_t length, int fragmentprogram, int metalshading)
{
	std::vector<std::string> statements;
	std::vector<std::string> lines;
	std::string source;
	std::string stripped;
	std::string declarations;
	std::string body;
	std::string results;
	std::string output;
	char *copy;
	size_t index;
	size_t comment;
	source.assign(programtext, length);
	lines = GfxAccelHostArbSplit(source, '\n');
	for (index = 0; index < lines.size(); index++) {
		comment = lines[index].find('#');
		if (comment != std::string::npos)
			lines[index] = lines[index].substr(0, comment);
		if (lines[index].compare(0, 2, "!!") == 0)
			continue;
		stripped += lines[index] + " ";
	}
	statements = GfxAccelHostArbSplit(stripped, ';');
	for (index = 0; index < statements.size(); index++) {
		const std::string &statement = statements[index];
		if (statement.empty() || statement == "END" || statement.compare(0, 7, "OPTION ") == 0)
			continue;
		if (statement.compare(0, 5, "TEMP ") == 0 || statement.compare(0, 6, "PARAM ") == 0 || statement.compare(0, 7, "ATTRIB ") == 0 || statement.compare(0, 7, "OUTPUT ") == 0) {
			if (!GfxAccelHostArbDeclaration(statement, fragmentprogram, &declarations, &results))
				return NULL;
			continue;
		}
		if (!GfxAccelHostArbInstruction(statement, fragmentprogram, &body))
			return NULL;
	}
	if (fragmentprogram && metalshading)
		output = GfxAccelHostArbFragmentPreludeMetal;
	else if (fragmentprogram)
		output = GfxAccelHostArbFragmentPrelude;
	else if (metalshading)
		output = GfxAccelHostArbVertexPreludeMetal;
	else
		output = GfxAccelHostArbVertexPrelude;
	output += declarations + body + results;
	if (fragmentprogram)
		output += "\toutputcolor = outputcolor + (input.color + input.tex0 + input.tex1 + input.tex2 + input.tex3) * Local[95].x;\n\treturn outputcolor;\n}\n";
	else
		output += "\treturn output;\n}\n";
	copy = (char *)malloc(output.size() + 1);
	if (copy)
		memcpy(copy, output.c_str(), output.size() + 1);
	return copy;
}
