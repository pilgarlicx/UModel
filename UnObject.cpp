#include "Core.h"
#include "UnCore.h"
#include "UnObject.h"
#include "UnPackage.h"


#define DEBUG_PROPS		1


/*-----------------------------------------------------------------------------
	UObject class
-----------------------------------------------------------------------------*/

UObject::~UObject()
{
//	printf("deleting %s\n", Name);
	// remove self from package export table
	if (Package)
	{
		assert(Package->ExportTable[PackageIndex].Object == this);
		Package->ExportTable[PackageIndex].Object = NULL;
		Package = NULL;
	}
}


/*-----------------------------------------------------------------------------
	UObject loading from package
-----------------------------------------------------------------------------*/

int              UObject::GObjBeginLoadCount = 0;
TArray<UObject*> UObject::GObjLoaded;


void UObject::BeginLoad()
{
	assert(GObjBeginLoadCount >= 0);
	GObjBeginLoadCount++;
}


void UObject::EndLoad()
{
	assert(GObjBeginLoadCount > 0);
	if (--GObjBeginLoadCount > 0)
		return;

	guard(UObject::EndLoad);
	// process GObjLoaded array
	// NOTE: while loading one array element, array may grow!
	for (int i = 0; i < GObjLoaded.Num(); i++)
	{
		UObject *Obj = GObjLoaded[i];
		guard(LoadObject);
		//!! should sort by packages
		UnPackage *Package = Obj->Package;
		Package->SetupReader(Obj->PackageIndex);
		printf("Loading %s %s from package %s\n", Obj->GetClassName(), Obj->Name, Package->SelfName);
		Obj->Serialize(*Package);
		// check for unread bytes
		if (!Package->IsStopper())
			appError("%s.Serialize(%s): %d unread bytes",
				Obj->GetClassName(), Obj->Name,
				Package->ArStopper - Package->ArPos);

		unguardf(("%s", Obj->Name));
	}
	GObjLoaded.Empty();
	unguard;
}


/*-----------------------------------------------------------------------------
	Properties support
-----------------------------------------------------------------------------*/

static bool SerializeStruc(FArchive &Ar, void *Data, const char *StrucName)
{
	guard(SerializeStruc);
#define STRUC_TYPE(name)				\
	if (!strcmp(StrucName, #name))		\
	{									\
		Ar << *((F##name*)Data);		\
		return true;					\
	}
	STRUC_TYPE(Vector)
	STRUC_TYPE(Rotator)
	STRUC_TYPE(Color)
	return false;
	unguardf(("%s", StrucName));
}


enum EPropType // hardcoded in Unreal
{
	PT_BYTE = 1,
	PT_INT,
	PT_BOOL,
	PT_FLOAT,
	PT_OBJECT,
	PT_NAME,
	PT_STRING,
	PT_CLASS,
	PT_ARRAY,
	PT_STRUCT,
	PT_VECTOR,
	PT_ROTATOR,
	PT_STR,
	PT_MAP,
	PT_FIXED_ARRAY
};


void UObject::Serialize(FArchive &Ar)
{
	guard(UObject::Serialize);
	// stack frame
//	assert(!(ObjectFlags & RF_HasStack));

	// property list
	while (true)
	{
		FName PropName;
		Ar << PropName;
		if (!strcmp(PropName, "None"))
			break;

		guard(ReadProperty);

		byte info;
		Ar << info;
		bool IsArray  = (info & 0x80) != 0;
		byte PropType = info & 0xF;
		// analyze 'size' field
		int  Size = 0;
		switch ((info >> 4) & 7)
		{
		case 0: Size = 1; break;
		case 1: Size = 2; break;
		case 2: Size = 4; break;
		case 3: Size = 12; break;
		case 4: Size = 16; break;
		case 5: Ar << *((byte*)&Size); break;
		case 6: Ar << *((word*)&Size); break;
		case 7: Ar << *((int *)&Size); break;
		}

		int ArrayIndex = 0;
		if (PropType != 3 && IsArray)	// 'bool' type has separate meaning of 'array' flag
		{
			// read array index
			byte b;
			Ar << b;
			if (b < 128)
				ArrayIndex = b;
			else
			{
				byte b2;
				Ar << b2;
				if (b & 0x40)			// really, (b & 0xC0) == 0xC0
				{
					byte b3, b4;
					Ar << b3 << b4;
					ArrayIndex = ((b << 24) | (b2 << 16) | (b3 << 8) | b4) & 0x3FFFFF;
				}
				else
					ArrayIndex = ((b << 8) | b2) & 0x3FFF;
			}
		}

		int StopPos = Ar.ArPos + Size;	// for verification; overrided below for some types

		const CPropInfo *Prop = FindProperty(PropName);
		if (!Prop)
		{
			appNotify("WARNING: Class \"%s\": property \"%s\" was not found", GetClassName(), *PropName);
			switch (PropType)
			{
			case PT_STRUCT:
				{
					// skip name (variable sized)
					FName tmp;
					Ar << tmp;
					// correct StopPos
					StopPos = Ar.ArPos + Size;
				}
				break;
			}
			// skip property data
			Ar.Seek(StopPos);
			// serialize other properties
			continue;
		}
		// verify array index
		if (ArrayIndex >= Prop->Count)
			appError("Class \"%s\": %s %s[%d]: serializing index %d",
				GetClassName(), Prop->TypeName, Prop->Name, Prop->Count, ArrayIndex);
		byte *value = (byte*)this + Prop->Offset;

#define TYPE(name) \
	if (strcmp(Prop->TypeName, name)) \
		appError("Property %s expected type %s but read %s", *PropName, Prop->TypeName, name)

#define PROP(type)		( ((type*)value)[ArrayIndex] )

#if DEBUG_PROPS
#	define PROP_DBG(fmt, value) \
		printf("  %s[%d] = " fmt "\n", *PropName, ArrayIndex, value);
#else
#	define PROP_DBG(fmt, value)
#endif

		switch (PropType)
		{
		case PT_BYTE:
			TYPE("byte");
			Ar << PROP(byte);
			PROP_DBG("%d", PROP(byte));
			break;

		case PT_INT:
			TYPE("int");
			Ar << PROP(int);
			PROP_DBG("%d", PROP(int));
			break;

		case PT_BOOL:
			TYPE("bool");
			PROP(bool) = IsArray;
			PROP_DBG("%s", PROP(bool) ? "true" : "false");
			break;

		case PT_FLOAT:
			TYPE("float");
			Ar << PROP(float);
			PROP_DBG("%g", PROP(float));
			break;

		case PT_OBJECT:
			TYPE("UObject*");
			Ar << PROP(UObject*);
			PROP_DBG("%s", PROP(UObject*) ? PROP(UObject*)->Name : "Null");
			break;

		case PT_NAME:
			TYPE("FName");
			Ar << PROP(FName);
			PROP_DBG("%s", *PROP(FName));
			break;

		case PT_CLASS:
			appError("Class property not implemented");
			break;

		case PT_ARRAY:
			appError("Array property not implemented");
			break;

		case PT_STRUCT:
			{
				assert(ArrayIndex == 0);	//!! implement structure arrays
				// read structure name
				FName StrucName;
				Ar << StrucName;
				StopPos = Ar.ArPos + Size;
				if (strcmp(Prop->TypeName+1, *StrucName))
					appError("Struc property %s expected type %s but read %s", *PropName, Prop->TypeName, *StrucName);
				if (SerializeStruc(Ar, value, StrucName))
				{
					PROP_DBG("(complex)", 0);
				}
				else
				{
					appNotify("WARNING: Unknown structure type: %s", *StrucName);
					Ar.Seek(StopPos);
				}
			}
			break;

		case PT_STR:
			appError("String property not implemented");
			break;

		case PT_MAP:
			appError("Map property not implemented");
			break;

		case PT_FIXED_ARRAY:
			appError("FixedArray property not implemented");
			break;

		// reserved, but not implemented in unreal:
		case PT_STRING:		//------  string  => used str
		case PT_VECTOR:	//------  vector  => used structure"Vector"
		case PT_ROTATOR:	//------  rotator => used structure"Rotator"
			appError("Unknown property");
			break;
		}
		//!!!!!!
//		assert(Ar.ArPos == StopPos);
		if (Ar.ArPos != StopPos) appNotify("ArPos-StopPos = %d", Ar.ArPos - StopPos);

		unguardf(("(%s.%s)", GetClassName(), *PropName));
	}
	unguard;
}


/*-----------------------------------------------------------------------------
	RTTI support
-----------------------------------------------------------------------------*/

static CClassInfo* GClasses    = NULL;
static int         GClassCount = 0;

void RegisterClasses(CClassInfo *Table, int Count)
{
	assert(GClasses == NULL);		// no multiple tables
	GClasses    = Table;
	GClassCount = Count;
}


UObject *CreateClass(const char *Name)
{
	for (int i = 0; i < GClassCount; i++)
		if (!strcmp(GClasses[i].Name, Name))
			return GClasses[i].Constructor();
	return NULL;
}


const CPropInfo *UObject::FindProperty(const CPropInfo *Table, int Count, const char *PropName)
{
	for (int i = 0; i < Count; i++, Table++)
		if (!strcmp(Table->Name, PropName))
			return Table;
	return NULL;
}
