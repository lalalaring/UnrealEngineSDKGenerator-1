#pragma once

#include <string>
#include <vector>

#include "Flags.hpp"
#include "../IGenerator.hpp"

class UObject;
class UEClass;

class UEObject
{
public:
	UEObject()
		: object(nullptr)
	{
	}
	UEObject(UObject *_object)
		: object(_object)
	{
	}

	bool IsValid() const
	{
		return object != nullptr;
	}

	size_t GetIndex() const;

	UEClass GetClass() const;

	UEObject GetOuter() const;

	std::string GetName() const;

	std::string GetFullName() const;

	std::string GetNameCPP() const;

	UEObject GetPackageObject() const;

	void* GetAddress() const;

	template<typename Base>
	Base Cast() const
	{
		return Base(object);
	}

	template<typename T>
	bool IsA() const;

	static UEClass StaticClass();

protected:
	UObject* object;
};

namespace std
{
	template<>
	struct hash<UEObject>
	{
		size_t operator()(const UEObject& obj) const
		{
			return std::hash<void*>()(obj.GetAddress());
		}
	};
}

inline bool operator==(const UEObject& lhs, const UEObject& rhs) { return rhs.GetAddress() == lhs.GetAddress(); }
inline bool operator!=(const UEObject& lhs, const UEObject& rhs) { return !(lhs == rhs); }

class UEField : public UEObject
{
public:
	using UEObject::UEObject;

	UEField GetNext() const;

	static UEClass StaticClass();
};

class UEEnum : public UEField
{
public:
	using UEField::UEField;

	std::vector<std::string> GetNames() const;

	static UEClass StaticClass();
};

class UEConst : public UEField
{
public:
	using UEField::UEField;

	std::string GetValue() const;

	static UEClass StaticClass();
};

class UEStruct : public UEField
{
public:
	using UEField::UEField;

	UEStruct GetSuper() const;
	
	UEField GetChildren() const;

	size_t GetPropertySize() const;

	static UEClass StaticClass();
};

class UEScriptStruct : public UEStruct
{
public:
	using UEStruct::UEStruct;

	static UEClass StaticClass();
};

class UEFunction : public UEStruct
{
public:
	using UEStruct::UEStruct;

	UEFunctionFlags GetFunctionFlags() const;

	static UEClass StaticClass();
};

class UEState : public UEStruct
{
public:
	using UEStruct::UEStruct;

	static UEClass StaticClass();
};

class UEClass : public UEState
{
public:
	using UEState::UEState;

	static UEClass StaticClass();
};

class UEProperty : public UEField
{
public:
	using UEField::UEField;

	size_t GetArrayDim() const;

	size_t GetElementSize() const;

	UEPropertyFlags GetPropertyFlags() const;

	size_t GetOffset() const;

	enum class PropertyType
	{
		Unknown,
		Primitive,
		PredefinedStruct,
		CustomStruct,
		Container
	};

	struct Info
	{
		PropertyType Type;
		size_t Size;
		bool CanBeReference;
		std::string CppType;

		static Info Create(PropertyType type, size_t size, bool reference, std::string&& cppType)
		{
			extern IGenerator* generator;

			return { type, size, reference, generator->GetOverrideType(cppType) };
		}
	};

	Info GetInfo() const;

	static UEClass StaticClass();
};

class UEPointerProperty : public UEProperty
{
public:
	using UEProperty::UEProperty;

	UEProperty::Info GetInfo() const;

	static UEClass StaticClass();
};

class UEByteProperty : public UEProperty
{
public:
	using UEProperty::UEProperty;

	bool IsEnum() const;

	UEEnum GetEnum() const;

	UEProperty::Info GetInfo() const;

	static UEClass StaticClass();
};

class UEIntProperty : public UEProperty
{
public:
	using UEProperty::UEProperty;

	UEProperty::Info GetInfo() const;

	static UEClass StaticClass();
};

class UEFloatProperty : public UEProperty
{
public:
	using UEProperty::UEProperty;

	UEProperty::Info GetInfo() const;

	static UEClass StaticClass();
};

class UEBoolProperty : public UEProperty
{
public:
	using UEProperty::UEProperty;

	size_t GetBitMask() const;

	UEProperty::Info GetInfo() const;

	static UEClass StaticClass();
};

class UEObjectProperty : public UEProperty
{
public:
	using UEProperty::UEProperty;

	UEProperty::Info GetInfo() const;

	static UEClass StaticClass();
};

class UEClassProperty : public UEObjectProperty
{
public:
	using UEObjectProperty::UEObjectProperty;

	UEClass GetMetaClass() const;

	UEProperty::Info GetInfo() const;

	static UEClass StaticClass();
};

class UEInterfaceProperty : public UEProperty
{
public:
	using UEProperty::UEProperty;

	UEClass GetInterfaceClass() const;

	UEProperty::Info GetInfo() const;

	static UEClass StaticClass();
};

class UENameProperty : public UEProperty
{
public:
	using UEProperty::UEProperty;

	UEProperty::Info GetInfo() const;

	static UEClass StaticClass();
};

class UEStructProperty : public UEProperty
{
public:
	using UEProperty::UEProperty;

	UEScriptStruct GetStruct() const;

	UEProperty::Info GetInfo() const;

	static UEClass StaticClass();
};

class UEStrProperty : public UEProperty
{
public:
	using UEProperty::UEProperty;

	UEProperty::Info GetInfo() const;

	static UEClass StaticClass();
};

class UEArrayProperty : public UEProperty
{
public:
	using UEProperty::UEProperty;

	UEProperty GetInner() const;

	UEProperty::Info GetInfo() const;

	static UEClass StaticClass();
};

class UEMapProperty : public UEProperty
{
public:
	using UEProperty::UEProperty;

	UEProperty GetKeyProperty() const;
	UEProperty GetValueProperty() const;

	UEProperty::Info GetInfo() const;

	static UEClass StaticClass();
};

class UEDelegateProperty : public UEProperty
{
public:
	using UEProperty::UEProperty;

	UEFunction GetSignatureFunction() const;

	UEProperty::Info GetInfo() const;

	static UEClass StaticClass();
};

class UEEnumProperty : public UEProperty
{
public:
	using UEProperty::UEProperty;

	UEEnum GetEnum() const;

	static UEClass StaticClass();
};

template<typename T>
bool UEObject::IsA() const
{
	auto cmp = T::StaticClass();
	if (!cmp.IsValid())
	{
		return false;
	}

	for (auto super = GetClass(); super.IsValid(); super = super.GetSuper().Cast<UEClass>())
	{
		if (super.object == cmp.object)
		{
			return true;
		}
	}

	return false;
}

template<>
inline bool UEObject::IsA<UEScriptStruct>() const
{
	//UE1 doesn't have ScriptStruct so we need to check a little bit more
	if (IsA<UEStruct>())
	{
		return !IsA<UEFunction>() && !IsA<UEState>();
	}

	return false;
}

template<>
inline bool UEObject::IsA<UEEnumProperty>() const
{
	//UE1 doesn't have an EnumProperty.

	return false;
}
