#include "Package.hpp"

#include <fstream>
#include <unordered_set>
#include "tinyformat.h"
#include "cpplinq.hpp"

#include "IGenerator.hpp"
#include "Logger.hpp"
#include "NameValidator.hpp"
#include "PatternFinder.hpp"
#include "ObjectsStore.hpp"
#include "Flags.hpp"
#include "PrintHelper.hpp"

/// <summary>
/// Compare two properties.
/// </summary>
/// <param name="lhs">The first property.</param>
/// <param name="rhs">The second property.</param>
/// <returns>true if the first property compares less, else false.</returns>
bool ComparePropertyLess(const UEProperty& lhs, const UEProperty& rhs)
{
	if (lhs.GetOffset() == rhs.GetOffset()
		&& lhs.IsA<UEBoolProperty>()
		&& rhs.IsA<UEBoolProperty>())
	{
		return lhs.Cast<UEBoolProperty>().GetBitMask() < rhs.Cast<UEBoolProperty>().GetBitMask();
	}
	
	return lhs.GetOffset() < rhs.GetOffset();
}

Package::Package(const UEObject& _packageObj, std::vector<UEObject>& _packageOrder, std::unordered_map<UEObject, bool>& _processedObjects)
	: packageObj(_packageObj),
	  packageOrder(_packageOrder),
	  processedObjects(_processedObjects)
{
}

void Package::Process()
{
	for (auto obj : ObjectsStore())
	{
		auto package = obj.Object.GetPackageObject();
		if (packageObj == package)
		{
			if (obj.Object.IsA<UEEnum>())
			{
				GenerateEnum(obj.Object.Cast<UEEnum>());
			}
			else if (obj.Object.IsA<UEConst>())
			{
				GenerateConst(obj.Object.Cast<UEConst>());
			}
			else if (obj.Object.IsA<UEClass>())
			{
				GeneratePrerequisites(obj.Object.Cast<UEClass>());
			}
			else if (obj.Object.IsA<UEScriptStruct>())
			{
				GeneratePrerequisites(obj.Object.Cast<UEScriptStruct>());
			}
		}
	}
}

bool Package::Save(const fs::path& path) const
{
	extern IGenerator* generator;

	using namespace cpplinq;

	//check if package is empty (no enums, structs or classes without members)
	if (generator->ShouldGenerateEmptyFiles()
		|| (from(enums) >> where([](auto&& e) { return !e.Values.empty(); }) >> any()
			|| from(scriptStructs) >> where([](auto&& s) { return !s.Members.empty() || !s.PredefinedMethods.empty(); }) >> any()
			|| from(classes) >> where([](auto&& c) {return !c.Members.empty() || !c.PredefinedMethods.empty() || !c.Methods.empty(); }) >> any()
		)
	)
	{
		SaveStructs(path);
		SaveClasses(path);
		SaveFunctions(path);

		return true;
	}
	
	Logger::Log("skip empty Package: %s", packageObj.GetName());

	return false;
}

bool Package::UpdatePackageOrder(const UEObject& package) const
{
	if (std::find(std::begin(packageOrder), std::end(packageOrder), packageObj) == std::end(packageOrder))
	{
		packageOrder.push_back(packageObj);
	}

	if (package != packageObj)
	{
		auto itPO = std::find(std::begin(packageOrder), std::end(packageOrder), package);
		auto itPTP = std::find(std::begin(packageOrder), std::end(packageOrder), packageObj);

		if (itPO == std::end(packageOrder))
		{
			packageOrder.insert(itPTP, package);
		}
		else if (itPO > itPTP)
		{
			packageOrder.insert(itPTP, package);
			//iterator may be invalid after the insert
			itPO = std::next(std::find(std::rbegin(packageOrder), std::rend(packageOrder), package)).base();
			packageOrder.erase(itPO);
		}

		return false;
	}
	return true;
}

void Package::GeneratePrerequisites(const UEObject& obj)
{
	if (!obj.IsValid())
	{
		return;
	}

	auto isClass = obj.IsA<UEClass>();
	auto isScriptStruct = obj.IsA<UEScriptStruct>();
	if (!isClass && !isScriptStruct)
	{
		return;
	}

	auto name = obj.GetName();
	if (name.find("Default__") != std::string::npos
		|| name.find("<uninitialized>") != std::string::npos
		|| name.find("PLACEHOLDER-CLASS") != std::string::npos)
	{
		return;
	}

	processedObjects[obj] |= false;

	auto classPackage = obj.GetPackageObject();
	if (!classPackage.IsValid())
	{
		return;
	}

	if (!UpdatePackageOrder(classPackage))
	{
		return;
	}

	if (processedObjects[obj] == false)
	{
		processedObjects[obj] = true;

		auto outer = obj.GetOuter();
		if (outer.IsValid() && outer != obj)
		{
			GeneratePrerequisites(outer);
		}

		auto structObj = obj.Cast<UEStruct>();

		auto super = structObj.GetSuper();
		if (super.IsValid() && super != obj)
		{
			GeneratePrerequisites(super);
		}

		GenerateMemberPrerequisites(structObj.GetChildren().Cast<UEProperty>());

		if (isClass)
		{
			GenerateClass(obj.Cast<UEClass>());
		}
		else
		{
			GenerateScriptStruct(obj.Cast<UEScriptStruct>());
		}
	}
}

void Package::GenerateMemberPrerequisites(const UEProperty& first)
{
	using namespace cpplinq;

	for (auto prop = first; prop.IsValid(); prop = prop.GetNext().Cast<UEProperty>())
	{
		if (prop.IsA<UEByteProperty>())
		{
			auto byteProperty = prop.Cast<UEByteProperty>();
			if (byteProperty.IsEnum())
			{
				UpdatePackageOrder(byteProperty.GetEnum().GetPackageObject());

				continue;
			}
		}

		auto info = prop.GetInfo();
		if (info.Type == UEProperty::PropertyType::CustomStruct)
		{
			GeneratePrerequisites(prop.Cast<UEStructProperty>().GetStruct());
		}
		else if (info.Type == UEProperty::PropertyType::Container)
		{
			std::vector<UEProperty> innerProperties;

			if (prop.IsA<UEArrayProperty>())
			{
				innerProperties.push_back(prop.Cast<UEArrayProperty>().GetInner());
			}
			else if (prop.IsA<UEMapProperty>())
			{
				auto mapProp = prop.Cast<UEMapProperty>();
				innerProperties.push_back(mapProp.GetKeyProperty());
				innerProperties.push_back(mapProp.GetValueProperty());
			}

			for (auto innerProp : from(innerProperties)
				>> where([](auto&& p) { return p.GetInfo().Type == UEProperty::PropertyType::CustomStruct; })
				>> experimental::container())
			{
				GeneratePrerequisites(innerProp.Cast<UEStructProperty>().GetStruct());
			}
		}
	}
}

Package::Member Package::Member::Unknown(size_t id, size_t offset, size_t size, std::string reason)
{
	Member ss;
	ss.Name = tfm::format("UnknownData%02d[0x%X]", id, size);
	ss.Type = "unsigned char";
	ss.Offset = offset;
	ss.Size = size;
	ss.Comment = std::move(reason);
	return ss;
}

void Package::GenerateScriptStruct(const UEScriptStruct& scriptStructObj)
{
	extern IGenerator* generator;

	ScriptStruct ss;
	ss.Name = scriptStructObj.GetName();
	ss.FullName = scriptStructObj.GetFullName();

	Logger::Log("ScriptStruct: %-100s - instance: 0x%P", ss.Name, scriptStructObj.GetAddress());

	ss.NameCpp = MakeValidName(scriptStructObj.GetNameCPP());
	ss.NameCppFull = "struct ";

	//some classes need special alignment
	auto alignment = generator->GetClassAlignas(ss.FullName);
	if (alignment != 0)
	{
		ss.NameCppFull += tfm::format("alignas(%d) ", alignment);
	}

	ss.NameCppFull += MakeUniqueCppName(scriptStructObj);

	ss.Size = scriptStructObj.GetPropertySize();
	ss.InheritedSize = 0;

	size_t offset = 0;

	auto super = scriptStructObj.GetSuper();
	if (super.IsValid() && super != scriptStructObj)
	{
		ss.InheritedSize = offset = super.GetPropertySize();

		ss.NameCppFull += " : public " + MakeUniqueCppName(super.Cast<UEScriptStruct>());
	}

	std::vector<UEProperty> properties;
	for (auto prop = scriptStructObj.GetChildren().Cast<UEProperty>(); prop.IsValid(); prop = prop.GetNext().Cast<UEProperty>())
	{
		if (prop.GetElementSize() > 0
			&& !prop.IsA<UEScriptStruct>()
			&& !prop.IsA<UEFunction>()
			&& !prop.IsA<UEEnum>()
			&& !prop.IsA<UEConst>()
		)
		{
			properties.push_back(prop);
		}
	}
	std::sort(std::begin(properties), std::end(properties), ComparePropertyLess);

	GenerateMembers(scriptStructObj, offset, properties, ss.Members);

	generator->GetPredefinedClassMethods(scriptStructObj.GetFullName(), ss.PredefinedMethods);

	scriptStructs.emplace_back(std::move(ss));
}

void Package::GenerateEnum(const UEEnum& enumObj)
{
	Enum e;
	e.Name = MakeUniqueCppName(enumObj);

	if (e.Name.find("Default__") != std::string::npos
		|| e.Name.find("PLACEHOLDER-CLASS") != std::string::npos)
	{
		return;
	}

	e.FullName = enumObj.GetFullName();

	std::unordered_map<std::string, int> conflicts;
	for (auto&& s : enumObj.GetNames())
	{
		auto clean = MakeValidName(std::forward<std::string>(s));

		auto it = conflicts.find(clean);
		if (it == std::end(conflicts))
		{
			e.Values.push_back(clean);
			conflicts[clean] = 1;
		}
		else
		{
			e.Values.push_back(clean + tfm::format("%02d", it->second));
			conflicts[clean]++;
		}
	}

	enums.emplace_back(std::move(e));
}

void Package::GenerateConst(const UEConst& constObj)
{
	//auto name = MakeValidName(constObj.GetName());
	auto name = MakeUniqueCppName(constObj);

	if (name.find("Default__") != std::string::npos
		|| name.find("PLACEHOLDER-CLASS") != std::string::npos)
	{
		return;
	}

	constants[name] = constObj.GetValue();
}

void Package::GenerateClass(const UEClass& classObj)
{
	extern IGenerator* generator;

	Class c;
	c.Name = classObj.GetName();
	c.FullName = classObj.GetFullName();

	Logger::Log("Class:        %-100s - instance: 0x%P", c.Name, classObj.GetAddress());

	c.NameCpp = MakeValidName(classObj.GetNameCPP());
	c.NameCppFull = "class " + c.NameCpp;

	c.Size = classObj.GetPropertySize();
	c.InheritedSize = 0;

	size_t offset = 0;

	auto super = classObj.GetSuper();
	if (super.IsValid() && super != classObj)
	{
		c.InheritedSize = offset = super.GetPropertySize();

		c.NameCppFull += " : public " + MakeValidName(super.GetNameCPP());
	}

	std::vector<IGenerator::PredefinedMember> predefinedStaticMembers;
	if (generator->GetPredefinedClassStaticMembers(c.FullName, predefinedStaticMembers))
	{
		for (auto&& prop : predefinedStaticMembers)
		{
			Member p;
			p.Offset = 0;
			p.Size = 0;
			p.Name = prop.Name;
			p.Type = "static " + prop.Type;
			c.Members.push_back(std::move(p));
		}
	}

	std::vector<IGenerator::PredefinedMember> predefinedMembers;
	if (generator->GetPredefinedClassMembers(c.FullName, predefinedMembers))
	{
		for (auto&& prop : predefinedMembers)
		{
			Member p;
			p.Offset = 0;
			p.Size = 0;
			p.Name = prop.Name;
			p.Type = prop.Type;
			p.Comment = "NOT AUTO-GENERATED PROPERTY";
			c.Members.push_back(std::move(p));
		}
	}
	else
	{
		std::vector<UEProperty> properties;
		for (auto prop = classObj.GetChildren().Cast<UEProperty>(); prop.IsValid(); prop = prop.GetNext().Cast<UEProperty>())
		{
			if (prop.GetElementSize() > 0
				&& !prop.IsA<UEScriptStruct>()
				&& !prop.IsA<UEFunction>()
				&& !prop.IsA<UEEnum>()
				&& !prop.IsA<UEConst>()
				&& (!super.IsValid()
					|| (super != classObj
						&& prop.GetOffset() >= super.GetPropertySize()
						)
					)
				)
			{
				properties.push_back(prop);
			}
		}
		std::sort(std::begin(properties), std::end(properties), ComparePropertyLess);

		GenerateMembers(classObj, offset, properties, c.Members);
	}

	generator->GetPredefinedClassMethods(c.FullName, c.PredefinedMethods);
	
	if (generator->ShouldUseStrings())
	{
		c.PredefinedMethods.push_back(IGenerator::PredefinedMethod::Inline(tfm::format(R"(	static UClass* StaticClass()
	{
		static auto ptr = UObject::FindClass(%s);
		return ptr;
	})", generator->ShouldXorStrings() ? tfm::format("_xor_(\"%s\")", c.FullName) : tfm::format("\"%s\"", c.FullName))));
	}
	else
	{
		c.PredefinedMethods.push_back(IGenerator::PredefinedMethod::Inline(tfm::format(R"(	static UClass* StaticClass()
	{
		static auto ptr = static_class<UClass*>(UObject::GetGlobalObjects().GetByIndex(%d));
		return ptr;
	})", classObj.GetIndex())));
	}

	GenerateMethods(classObj, c.Methods);

	//search virtual functions
	IGenerator::VirtualFunctionPatterns patterns;
	if (generator->GetVirtualFunctionPatterns(c.FullName, patterns))
	{
		auto vtable = *reinterpret_cast<uintptr_t**>(classObj.GetAddress());

		size_t methodCount = 0;
		while (true)
		{
			MEMORY_BASIC_INFORMATION mbi;
			auto res = VirtualQuery(reinterpret_cast<const void*>(vtable[methodCount]), &mbi, sizeof(mbi));
			if (res == 0 || (mbi.Protect != PAGE_EXECUTE_READWRITE && mbi.Protect != PAGE_EXECUTE_READ))
			{
				break;
			}
			++methodCount;
		}

		for (auto&& pattern : patterns)
		{
			for (auto i = 0u; i < methodCount; ++i)
			{
				if (vtable[i] != 0 && FindPattern(vtable[i], std::get<2>(pattern), reinterpret_cast<const unsigned char*>(std::get<0>(pattern)), std::get<1>(pattern)) != -1)
				{
					c.PredefinedMethods.push_back(IGenerator::PredefinedMethod::Inline(tfm::format(std::get<3>(pattern), i)));
					break;
				}
			}
		}
	}

	classes.emplace_back(std::move(c));
}

void Package::GenerateMembers(const UEStruct& structObj, size_t offset, const std::vector<UEProperty>& properties, std::vector<Member>& members) const
{
	extern IGenerator* generator;

	std::unordered_map<std::string, size_t> uniqueMemberNames;
	size_t unknownDataCounter = 0;

	for (auto& prop : properties)
	{
		if (offset < prop.GetOffset())
		{
			auto size = prop.GetOffset() - offset;
			if (size >= generator->GetGlobalMemberAlignment())
			{
				members.emplace_back(Member::Unknown(unknownDataCounter++, offset, size, "MISSED OFFSET"));
			}
		}

		auto info = prop.GetInfo();
		if (info.Type != UEProperty::PropertyType::Unknown)
		{
			Member sp;
			sp.Offset = prop.GetOffset();
			sp.Size = info.Size;

			sp.Type = info.CppType;
			sp.Name = MakeValidName(prop.GetName());

			auto it = uniqueMemberNames.find(sp.Name);
			if (it == std::end(uniqueMemberNames))
			{
				uniqueMemberNames[sp.Name] = 1;
			}
			else
			{
				++uniqueMemberNames[sp.Name];
				sp.Name += tfm::format("%02d", it->second);
			}

			if (prop.GetArrayDim() > 1)
			{
				sp.Name += tfm::format("[0x%X]", prop.GetArrayDim());
			}

			if (prop.IsA<UEBoolProperty>())
			{
				sp.Name += " : 1";
			}

			sp.Flags = static_cast<size_t>(prop.GetPropertyFlags());
			sp.FlagsString = StringifyFlags(prop.GetPropertyFlags());

			members.emplace_back(std::move(sp));

			auto sizeMismatch = static_cast<int>(prop.GetElementSize() * prop.GetArrayDim()) - static_cast<int>(info.Size * prop.GetArrayDim());
			if (sizeMismatch > 0)
			{
				members.emplace_back(Member::Unknown(unknownDataCounter++, offset, sizeMismatch, "FIX WRONG TYPE SIZE OF PREVIUS PROPERTY"));
			}
		}
		else
		{
			auto info2 = prop.GetInfo();
			auto size = prop.GetElementSize() * prop.GetArrayDim();
			members.emplace_back(Member::Unknown(unknownDataCounter++, offset, size, "UNKNOWN PROPERTY: " + prop.GetFullName()));
		}

		offset = prop.GetOffset() + (prop.GetElementSize() * prop.GetArrayDim());
	}

	if (offset < structObj.GetPropertySize())
	{
		auto size = structObj.GetPropertySize() - offset;
		if (size >= generator->GetGlobalMemberAlignment())
		{
			members.emplace_back(Member::Unknown(unknownDataCounter++, offset, size, "MISSED OFFSET"));
		}
	}
}

void Package::GenerateMethods(const UEClass& classObj, std::vector<Method>& methods) const
{
	extern IGenerator* generator;

	//some classes (AnimBlueprintGenerated...) have multiple members with the same name, so filter them out
	std::unordered_set<std::string> uniqueMethods;

	for (auto prop = classObj.GetChildren().Cast<UEProperty>(); prop.IsValid(); prop = prop.GetNext().Cast<UEProperty>())
	{
		if (prop.IsA<UEFunction>())
		{
			auto function = prop.Cast<UEFunction>();

			Method m;
			m.Index = function.GetIndex();
			m.FullName = function.GetFullName();
			m.Name = MakeValidName(function.GetName());

			if (uniqueMethods.find(m.FullName) != std::end(uniqueMethods))
			{
				continue;
			}
			uniqueMethods.insert(m.FullName);

			m.IsNative = function.GetFunctionFlags() & UEFunctionFlags::FUNC_Native;
			m.IsStatic = function.GetFunctionFlags() & UEFunctionFlags::FUNC_Static;
			m.FlagsString = StringifyFlags(function.GetFunctionFlags());

			std::vector<std::pair<UEProperty, Method::Parameter>> parameters;

			std::unordered_map<std::string, size_t> unique;
			for (auto param = function.GetChildren().Cast<UEProperty>(); param.IsValid(); param = param.GetNext().Cast<UEProperty>())
			{
				if (param.GetElementSize() == 0)
				{
					continue;
				}

				if (param.IsA<UEByteProperty>())
				{
					auto byteProperty = param.Cast<UEByteProperty>();
					if (byteProperty.IsEnum())
					{
						UpdatePackageOrder(byteProperty.GetEnum().GetPackageObject());
					}
				}

				auto info = param.GetInfo();
				if (info.Type != UEProperty::PropertyType::Unknown)
				{
					using Type = Method::Parameter::Type;

					Method::Parameter p;

					if (!Method::Parameter::MakeType(param.GetPropertyFlags(), p.ParamType))
					{
						//child isn't a parameter
						continue;
					}

					p.PassByReference = false;
					p.Name = MakeValidName(param.GetName());

					auto it = unique.find(p.Name);
					if (it == std::end(unique))
					{
						unique[p.Name] = 1;
					}
					else
					{
						++unique[p.Name];

						p.Name += tfm::format("%02d", it->second);
					}

					p.FlagsString = StringifyFlags(param.GetPropertyFlags());

					p.CppType = info.CppType;
					if (param.IsA<UEBoolProperty>())
					{
						p.CppType = generator->GetOverrideType("bool");
					}
					switch (p.ParamType)
					{
						case Type::Default:
							if (prop.GetArrayDim() > 1)
							{
								p.CppType = p.CppType + "*";
							}
							else if (info.CanBeReference)
							{
								p.PassByReference = true;
							}
							break;
					}

					parameters.emplace_back(std::make_pair(prop, std::move(p)));
				}
			}
			
			std::sort(std::begin(parameters), std::end(parameters), [](auto&& lhs, auto&& rhs) { return ComparePropertyLess(lhs.first, rhs.first); });

			for (auto& param : parameters)
			{
				m.Parameters.emplace_back(std::move(param.second));
			}

			methods.emplace_back(std::move(m));
		}
	}
}

void Package::SaveStructs(const fs::path& path) const
{
	extern IGenerator* generator;

	std::ofstream os(path / GenerateFileName(FileContentType::Structs, packageObj));

	PrintFileHeader(os, true);

	if (!constants.empty())
	{
		PrintSectionHeader(os, "Constants");
		for (auto&& c : constants) { PrintConstant(os, c); }

		os << "\n";
	}

	if (!enums.empty())
	{
		PrintSectionHeader(os, "Enums");
		for (auto&& e : enums) { PrintEnum(os, e); os << "\n"; }

		os << "\n";
	}

	if (!scriptStructs.empty())
	{
		PrintSectionHeader(os, "Script Structs");
		for (auto&& s : scriptStructs) { PrintStruct(os, s); os << "\n"; }
	}

	PrintFileFooter(os);
}

void Package::SaveClasses(const fs::path& path) const
{
	extern IGenerator* generator;

	std::ofstream os(path / GenerateFileName(FileContentType::Classes, packageObj));

	PrintFileHeader(os, true);

	if (!classes.empty())
	{
		PrintSectionHeader(os, "Classes");
		for (auto&& c : classes) { PrintClass(os, c); os << "\n"; }
	}

	PrintFileFooter(os);
}

void Package::SaveFunctions(const fs::path& path) const
{
	extern IGenerator* generator;

	using namespace cpplinq;

	if (generator->ShouldGenerateFunctionParametersFile())
	{
		SaveFunctionParameters(path);
	}

	std::ofstream os(path / GenerateFileName(FileContentType::Functions, packageObj));

	PrintFileHeader(os, { "\"../SDK.hpp\"" }, false);

	PrintSectionHeader(os, "Functions");

	for (auto&& s : scriptStructs)
	{
		for (auto&& m : s.PredefinedMethods)
		{
			if (m.MethodType != IGenerator::PredefinedMethod::Type::Inline)
			{
				os << m.Body << "\n\n";
			}
		}
	}

	for (auto&& c : classes)
	{
		for (auto&& m : c.PredefinedMethods)
		{
			if (m.MethodType != IGenerator::PredefinedMethod::Type::Inline)
			{
				os << m.Body << "\n\n";
			}
		}
		
		for (auto&& m : c.Methods)
		{
			//Method Info
			os << "// " << m.FullName << "\n"
				<< "// (" << m.FlagsString << ")\n";
			if (!m.Parameters.empty())
			{
				os << "// Parameters:\n";
				for (auto&& param : m.Parameters)
				{
					tfm::format(os, "// %-30s %-30s (%s)\n", param.CppType, param.Name, param.FlagsString);
				}
			}

			os << "\n";
			os << BuildMethodSignature(m, c, false) << "\n";
			os << BuildMethodBody(c, m) << "\n\n";
		}
	}

	PrintFileFooter(os);
}

void Package::SaveFunctionParameters(const fs::path& path) const
{
	using namespace cpplinq;

	std::ofstream os(path / GenerateFileName(FileContentType::FunctionParameters, packageObj));

	PrintFileHeader(os, { "\"../SDK.hpp\"" }, true);

	PrintSectionHeader(os, "Parameters");

	for (auto&& c : classes)
	{
		for (auto&& m : c.Methods)
		{
			os << "// " << m.FullName << "\n";
			tfm::format(os, "struct %s_%s_Params\n{\n", c.NameCpp, m.Name);
			for (auto&& param : m.Parameters)
			{
				tfm::format(os, "\t%-50s %-58s// (%s)\n", param.CppType, param.Name + ";", param.FlagsString);
			}
			os << "};\n\n";
		}
	}

	PrintFileFooter(os);
}

void Package::PrintConstant(std::ostream& os, const std::pair<std::string, std::string>& c) const
{
	tfm::format(os, "#define CONST_%-50s %s\n", c.first, c.second);
}

void Package::PrintEnum(std::ostream& os, const Enum& e) const
{
	using namespace cpplinq;

	os << "// " << e.FullName << "\nenum class " << e.Name << "\n{\n";
	os << (from(e.Values)
		>> select([](auto&& name, auto&& i) { return tfm::format("\t%-30s = %d", name, i); })
		>> concatenate(",\n"))
		<< "\n};\n\n";
}

void Package::PrintStruct(std::ostream& os, const ScriptStruct& ss) const
{
	using namespace cpplinq;

	os << "// " << ss.FullName << "\n// ";
	if (ss.InheritedSize)
	{
		os << tfm::format("0x%04X (0x%04X - 0x%04X)\n", ss.Size - ss.InheritedSize, ss.Size, ss.InheritedSize);
	}
	else
	{
		os << tfm::format("0x%04X\n", ss.Size);
	}

	os << ss.NameCppFull << "\n{\n";

	//Member
	os << (from(ss.Members)
		>> select([](auto&& m) {
				return tfm::format("\t%-50s %-58s// 0x%04X(0x%04X)", m.Type, m.Name + ";", m.Offset, m.Size)
					+ (!m.Comment.empty() ? " " + m.Comment : "")
					+ (!m.FlagsString.empty() ? " (" + m.FlagsString + ")" : "");
			})
		>> concatenate("\n"))
		<< "\n";

	//Predefined Methods
	if (!ss.PredefinedMethods.empty())
	{
		os << "\n";
		for (auto&& m : ss.PredefinedMethods)
		{
			if (m.MethodType == IGenerator::PredefinedMethod::Type::Inline)
			{
				os << m.Body;
			}
			else
			{
				os << "\t" << m.Signature << ";";
			}
			os << "\n\n";
		}
	}

	os << "};\n";
}

void Package::PrintClass(std::ostream& os, const Class& c) const
{
	using namespace cpplinq;
	
	os << "// " << c.FullName << "\n// ";
	if (c.InheritedSize)
	{
		tfm::format(os, "0x%04X (0x%04X - 0x%04X)\n", c.Size - c.InheritedSize, c.Size, c.InheritedSize);
	}
	else
	{
		tfm::format(os, "0x%04X\n", c.Size);
	}

	os << c.NameCppFull << "\n{\npublic:\n";

	//Member
	for (auto&& m : c.Members)
	{
		tfm::format(os, "\t%-50s %-58s// 0x%04X(0x%04X)", m.Type, m.Name + ";", m.Offset, m.Size);
		if (!m.Comment.empty())
		{
			os << " " << m.Comment;
		}
		if (!m.FlagsString.empty())
		{
			os << " (" << m.FlagsString << ")";
		}
		os << "\n";
	}

	//Predefined Methods
	if (!c.PredefinedMethods.empty())
	{
		os << "\n";
		for (auto&& m : c.PredefinedMethods)
		{
			if (m.MethodType == IGenerator::PredefinedMethod::Type::Inline)
			{
				os << m.Body;
			}
			else
			{
				os << "\t" << m.Signature << ";";
			}

			os << "\n\n";
		}
	}

	//Methods
	if (!c.Methods.empty())
	{
		os << "\n";
		for (auto&& m : c.Methods)
		{
			os << "\t" << BuildMethodSignature(m, {}, true) << ";\n";
		}
	}

	os << "};\n\n";
}

std::string Package::BuildMethodSignature(const Method& m, const Class& c, bool inHeader) const
{
	extern IGenerator* generator;

	using namespace cpplinq;
	using Type = Method::Parameter::Type;

	std::ostringstream ss;

	if (m.IsStatic && inHeader && !generator->ShouldConvertStaticMethods())
	{
		ss << "static ";
	}

	//Return Type
	auto retn = from(m.Parameters) >> where([](auto&& param) { return param.ParamType == Type::Return; });
	if (retn >> any())
	{
		ss << (retn >> first()).CppType;
	}
	else
	{
		ss << "void";
	}
	ss << " ";

	if (!inHeader)
	{
		ss << c.NameCpp << "::";
	}
	if (m.IsStatic && generator->ShouldConvertStaticMethods())
	{
		ss << "STATIC_";
	}
	ss << m.Name;

	//Parameters
	ss << "(";
	ss << (from(m.Parameters)
		>> where([](auto&& param) { return param.ParamType != Type::Return; })
		>> orderby([](auto&& param) { return param.ParamType; })
		>> select([](auto&& param) { return (param.PassByReference ? "const " : "") + param.CppType + (param.PassByReference ? "& " : param.ParamType == Type::Out ? "* " : " ") + param.Name; })
		>> concatenate(", "));
	ss << ")";

	return ss.str();
}

std::string Package::BuildMethodBody(const Class& c, const Method& m) const
{
	extern IGenerator* generator;

	using namespace cpplinq;
	using Type = Method::Parameter::Type;

	std::ostringstream ss;

	//Function Pointer
	ss << "{\n\tstatic auto fn";

	if (generator->ShouldUseStrings())
	{
		ss << " = UObject::FindObject<UFunction>(";

		if (generator->ShouldXorStrings())
		{
			ss << "_xor_(\"" << m.FullName << "\")";
		}
		else
		{
			ss << "\"" << m.FullName << "\"";
		}

		ss << ");\n\n";
	}
	else
	{
		ss << " = static_cast<UFunction*>(UObject::GetGlobalObjects().GetByIndex(" << m.Index << "));\n\n";
	}

	//Parameters
	if (generator->ShouldGenerateFunctionParametersFile())
	{
		ss << "\t" << c.NameCpp << "_" << m.Name << "_Params params;\n";
	}
	else
	{
		ss << "\tstruct\n\t{\n";
		for (auto&& param : m.Parameters)
		{
			tfm::format(ss, "\t\t%-30s %s;\n", param.CppType, param.Name);
		}
		ss << "\t} params;\n";
	}

	auto defaultParameters = from(m.Parameters) >> where([](auto&& param) { return param.ParamType == Type::Default; });
	if (defaultParameters >> any())
	{
		for (auto&& param : defaultParameters >> experimental::container())
		{
			ss << "\tparams." << param.Name << " = " << param.Name << ";\n";
		}
	}

	ss << "\n";

	//Function Call
	ss << "\tauto flags = fn->FunctionFlags;\n";
	if (m.IsNative)
	{
		ss << "\tfn->FunctionFlags |= 0x" << tfm::format("%X", static_cast<std::underlying_type_t<UEFunctionFlags>>(UEFunctionFlags::FUNC_Native)) << ";\n";
	}

	ss << "\n";

	if (m.IsStatic && !generator->ShouldConvertStaticMethods())
	{
		ss << "\tstatic auto defaultObj = StaticClass()->CreateDefaultObject();\n";
		ss << "\tdefaultObj->ProcessEvent(fn, &params);\n\n";
	}
	else
	{
		ss << "\tUObject::ProcessEvent(fn, &params);\n\n";
	}

	ss << "\tfn->FunctionFlags = flags;\n";

	//Out Parameters
	auto out = from(m.Parameters) >> where([](auto&& param) { return param.ParamType == Type::Out; });
	if (out >> any())
	{
		ss << "\n";

		for (auto&& param : out >> experimental::container())
		{
			ss << "\tif (" << param.Name << " != nullptr)\n";
			ss << "\t\t*" << param.Name << " = params." << param.Name << ";\n";
		}
	}

	//Return Value
	auto retn = from(m.Parameters) >> where([](auto&& param) { return param.ParamType == Type::Return; });
	if (retn >> any())
	{
		ss << "\n\treturn params." << (retn >> first()).Name << ";\n";
	}

	ss << "}\n";

	return ss.str();
}
