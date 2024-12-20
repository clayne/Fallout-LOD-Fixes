#include "nvse/PluginAPI.h"
#include "GameData.hpp"
#include "stack"
#include "Allocator/BSMemory.hpp"

BS_ALLOCATORS

NVSEInterface* g_nvseInterface{};
IDebugLog	   gLog("logs\\LOD Fixes.log");

#define LOGGING 0
#define TREELOD 0

#if LOGGING
#define DEBUG_MSG(...) _MESSAGE(__VA_ARGS__)
#else
#define DEBUG_MSG(...)
#endif

bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info) {
	info->infoVersion   = PluginInfo::kInfoVersion;
	info->name          = "LOD Fixes";
    info->version       = 132;
	return !nvse->isEditor;
}

static bool bUseSpecular = false;
static bool bAllowForceUpdates = true;
static bool bTreeLODPatches = false;
static UInt32 uiSpecularShine = 65;

NiUpdateData NiUpdateData::kDefaultUpdateData = NiUpdateData();

std::vector<NiPointer<NiAVObject>> kAnimatedLODObjects;

void BGSDistantObjectBlock::Prepare() {
    bool bUseNormalLOD = !bPrepared || spShape && spPreviousShape && spShape != spPreviousShape;
    bool bUseStinger = !bPrepared || spStingerBlock && spPreviousBlock && spStingerBlock != spPreviousBlock;

    NiDX9Renderer* pRenderer = NiDX9Renderer::GetSingleton();
    if (bUseNormalLOD && spShape.m_pObject) {
        UInt32 uiChildCount = spBlock->GetChildCount();

        for (UInt32 i = 0; i < uiChildCount; i++) {
            NiAVObject* pChild = spBlock->GetAt(i);
            if (IS_SEGMENTEDTRISHAPE(pChild)) {
                BSSegmentedTriShape* pShape = static_cast<BSSegmentedTriShape*>(pChild);
                pShape->RemoveProperty(NiProperty::ALPHA);

                NiAlphaProperty* pAlphaProp = NiAlphaProperty::CreateObject();

                pAlphaProp->m_usFlags.Clear();
                pAlphaProp->SetAlphaTesting(true);
                pAlphaProp->SetAlphaBlending(false);
                pAlphaProp->SetTestRef(128u);
                pAlphaProp->SetTestMode(NiAlphaProperty::TEST_GREATER);
                pShape->AddProperty(pAlphaProp);

                if (bUseSpecular) {
                    static NiPointer<NiMaterialProperty> spDefaultMaterial = NiMaterialProperty::CreateObject();
                    spDefaultMaterial->m_fShine = uiSpecularShine;
                    pShape->AddProperty(spDefaultMaterial);
                }

                BSShaderProperty* pShaderProp = static_cast<BSShaderProperty*>(pShape->GetProperty(NiProperty::SHADE));
                pShaderProp->SetFlag(BSShaderProperty::BSSP_LOD_BUILDING, true);
                pShaderProp->SetFlag(BSShaderProperty::BSSP_LOD_LANDSCAPE, false);
                pShaderProp->SetFlag(BSShaderProperty::BSSP_SPECULAR, bUseSpecular); // vanilla is false
                BSShaderProperty* p30ShaderProp = pShaderProp->PickShader(pShape, 0, 1);
                if (p30ShaderProp) {
                    pShape->RemoveProperty(NiProperty::SHADE);
                    pShaderProp = p30ShaderProp;
                    pShape->AddProperty(p30ShaderProp);
                }
                BSShader* pShader = BSShaderManager::GetShader(pShaderProp->uiShaderIndex);
                pShape->m_pShader = pShader;
                pShape->UpdatePropertiesUpward();
                pShape->Update(NiUpdateData::kDefaultUpdateData);
                pShape->SetFixedBound(true);

                pTerrainNode->UpdateBlockVisibility(false);

                NiGeometryData* pModelData = pShape->GetModelData();
                pModelData->SetKeepFlags(NiGeometryData::KEEP_NONE);
                pModelData->SetConsistency(NiGeometryData::STATIC);

                pRenderer->PrecacheGeometry(pShape, 0, 0, pShader->GetShaderDeclaration(pShape, pShaderProp));
                pRenderer->PerformPrecache();
            }
            else if (IS_NODE(pChild)) {
                NiNode* pNode = static_cast<NiNode*>(pChild);
                pNode->SetAppCulled(false);
                CdeclCall(0x4B5D10, pNode); // Remove debug markers
                pNode->CreateWorldBoundIfMissing();
                pNode->UpdatePropertiesUpward();
                pNode->Update(NiUpdateData::kDefaultUpdateData);
                BSShaderManager::AssignShaders(pNode, false, false);
            }
        }

        if (NiNode::HasControllers(spBlock)) {
            CdeclCall(0xA6D2D0, spBlock); // Start animations
            DEBUG_MSG("Adding animated object %x, ref count %i + 1", spBlock, spBlock->m_uiRefCount);
            kAnimatedLODObjects.push_back(spBlock.m_pObject);
        }

        spBlock->UpdateWorldBound();
        bPrepared = true;
    }

    if (bUseStinger && spStingerBlock.m_pObject) {
        PrepareStinger(spStingerBlock.m_pObject);
        pRenderer->PerformPrecache();
        bPrepared = true;
    }
}

void BGSTerrainNode::UpdateBlockVisibility(bool) {
    TES* pTES = TES::GetSingleton();
    TESWorldSpace* pWorldspace = TES::GetWorldSpace();
    if (!pObjectBlock)
        return;

    BSSegmentedTriShape* pSegmentedShape = pObjectBlock->spShape;
    if (pSegmentedShape) {
        BSMultiBoundNode* pBlock = pObjectBlock->GetBlock(false);
        if (IsPlayerInRange()) {
            for (SInt32 x = 0; x < uiLODLevel; ++x) {
                for (SInt32 y = 0; y < uiLODLevel; ++y) {
                    UInt32 uiLevel = y + x * uiLODLevel;

                    TESObjectCELL* pCell = pWorldspace->GetCellAtCoord(x + GetCellX(), y + GetCellY());
                    bool bIsLoaded = TES::IsCellLoaded(pCell, true);

                    for (UInt32 uiObject = 0; uiObject < pBlock->GetArrayCount(); uiObject++) {
                        NiAVObject* pChild = pBlock->GetAt(uiObject);
                        if (!pChild || !IS_SEGMENTEDTRISHAPE(pChild))
                            continue;

                        // Intentional overwrite - spShape is a child of the multibound node
                        // We are handling multiple segmented trishapes instead of just one
                        pSegmentedShape = static_cast<BSSegmentedTriShape*>(pChild);
                        if (uiLevel >= pSegmentedShape->GetNumSegments())
                            continue;

                        if (bIsLoaded && !pSegmentedShape->IsSegmentEmpty(uiLevel) && pCell->bCanHideLOD)
                            pSegmentedShape->DisableSegment(uiLevel);
                        else
                            pSegmentedShape->EnableSegment(uiLevel);
                    }
                }
            }

            // Update visibility of all children regardless of LOD level
            for (UInt32 uiObject = 0; uiObject < pBlock->GetArrayCount(); uiObject++) {
                NiAVObject* pChild = pBlock->GetAt(uiObject);
                if (!pChild)
                    continue;

                if (IS_SEGMENTEDTRISHAPE(pChild)) {
                    pSegmentedShape = static_cast<BSSegmentedTriShape*>(pChild);
                    pSegmentedShape->UpdateDrawData();
                }
                else if (!pChild->GetIgnoreFade()) {
                    NiPoint3 kPos = pChild->m_kWorld.m_Translate;
                    TESObjectCELL* pCell = pWorldspace->GetCellAtCoord(int(kPos.x) >> 12, int(kPos.y) >> 12);
                    if (!pCell)
                        continue;

                    bool bIsLoaded = TES::IsCellLoaded(pCell, true);
                    pChild->SetAppCulled(bIsLoaded);
                    pChild->Update(NiUpdateData::kDefaultUpdateData);
                }
            }
        }
        else {
            for (UInt32 uiObject = 0; uiObject < pBlock->GetArrayCount(); uiObject++) {
                NiAVObject* pChild = pBlock->GetAt(uiObject);
                if (!pChild)
                    continue;

                if (IS_SEGMENTEDTRISHAPE(pChild))
                    static_cast<BSSegmentedTriShape*>(pChild)->EnableAllSegments();
                else
                    pChild->SetAppCulled(false);
            }
        }
    }

    BSMultiBoundNode* pStingerBound = pObjectBlock->GetBlock(true);
    if (!pStingerBound)
        return;

    if (IsPlayerInRange())
        pObjectBlock->ToggleVisibilityRecurse(pStingerBound, uiLODLevel, GetCellX(), GetCellY());
    else
        pObjectBlock->ShowRecurse(pStingerBound);
}

#if TREELOD
static UInt32 GetRelativeFormID(UInt32 auiFormID) {
    return auiFormID & 0xFFFFFF;
}

static UInt32 GetLODFormID(UInt32 auiFormID) {
    UInt32 uiRelativeFormID = GetRelativeFormID(auiFormID);
    return uiRelativeFormID | (1u << 24);
}

void __fastcall BGSDistantTreeBlock::HideLOD(BGSDistantTreeBlock* apThis, void*, UInt32 aID, bool abRegisterFormID) {
    BGSDistantTreeBlock::InstanceData* pInstance = nullptr;
    UInt32 uiUsedFormID = aID;
    bool bTreeFound = false;

    DEBUG_MSG("\n===== Hiding trees =====");
    if (apThis->kInstanceMap.m_uiCount == 0) {
        DEBUG_MSG("Instance map is empty!");
    }
    else {
        bTreeFound = apThis->kInstanceMap.GetAt(uiUsedFormID, pInstance);
		if (!bTreeFound) {
			DEBUG_MSG("Tree %08X not found - trying LOD FormID", aID);
			uiUsedFormID = GetLODFormID(aID);
			bTreeFound = apThis->kInstanceMap.GetAt(uiUsedFormID, pInstance);
		}

		if (!bTreeFound) {
			DEBUG_MSG("Tree %08X not found - trying relative FormID", aID);
			uiUsedFormID = GetRelativeFormID(aID);
			bTreeFound = apThis->kInstanceMap.GetAt(uiUsedFormID, pInstance);
		}

        if (!bTreeFound) {
            DEBUG_MSG("Tree %08X not found - trying relative LOD FormID", aID);
            uiUsedFormID = GetLODFormID(uiUsedFormID);
            bTreeFound = apThis->kInstanceMap.GetAt(uiUsedFormID, pInstance);
        }
#if LOGGING
		if (!bTreeFound) {
			DEBUG_MSG("Tree %08X not found - giving up. Tree LOD for this reference does not exist, despite it having the tree LOD flag.", aID);
		}
#endif
    }

    if (bTreeFound) {
        DEBUG_MSG("Hiding LOD for tree %08X", aID);
        pInstance->bHidden = true;
        (*apThis->kTreeGroups.GetAt(pInstance->uiTreeGroupIndex))->bShaderPropertyUpToDate = false;
    }
    else if (abRegisterFormID) {
        DEBUG_MSG("Tree %08X not found - adding FormID to array", aID);
        ThisStdCall(0x7CB2E0, &apThis->kFormIDs, &aID); // Append
    }
    else {
        DEBUG_MSG("Tree %08X not found", aID);
    }
}

static void __fastcall BSMap__SetAt(void* apThis, void*, UInt32 auiFormID, BGSDistantTreeBlock::InstanceData* apInstanceData) {
    UInt32 uiFormID = auiFormID;
    UInt32 uiIndex = uiFormID >> 24;
    if (uiIndex > 1)
        uiFormID = GetLODFormID(auiFormID);

#if LOGGING
    if (auiFormID == uiFormID)
        DEBUG_MSG("\n ===== Registering tree %08X at %f, %f, %f =====", auiFormID, apInstanceData->kPos.x, apInstanceData->kPos.y, apInstanceData->kPos.z);
    else
        DEBUG_MSG("\n ===== Registering corrected tree %08X (was %08X) at %f, %f, %f =====", uiFormID, auiFormID, apInstanceData->kPos.x, apInstanceData->kPos.y, apInstanceData->kPos.z);
#endif
    ThisStdCall(0x6F9A80, apThis, uiFormID, apInstanceData);
}


#if LOGGING
static void __fastcall BGSDistantTreeBlockLoadTask__Run(void* apThis) {
    DEBUG_MSG("\n===== Loading trees =====");
    ThisStdCall(0x6F9570, apThis);
}

static BGSDistantTreeBlock* __fastcall BGSDistantTreeBlock__BGSDistantTreeBlock(BGSDistantTreeBlock* apThis, void*, BGSTerrainNode* apNode, int lodLevel, __int16 x, __int16 y) {
    DEBUG_MSG("\n===== Creating BGSDistantTreeBlock =====");
    return ThisStdCall<BGSDistantTreeBlock*>(0x6F7540, apThis, apNode, lodLevel, x, y);
}

static TESFile* __fastcall GetFile(TESForm* apThis, void*, SInt32 i) {
    TESFile* pFile = apThis->GetFile(i);
	DEBUG_MSG("\nGetting file %i for form %08X: \"%s\"", i, apThis->uiFormID, pFile ? pFile->cFilename : "null");
	bool bHasMasters = pFile && pFile->HasMasters();
	DEBUG_MSG("File has masters: %s", bHasMasters ? "true" : "false");
    UInt32 uiFormID = GetRelativeFormID(apThis->uiFormID) | (bHasMasters << 24);
	DEBUG_MSG("LOD Form ID: %08X", uiFormID);
	return pFile;
}
#endif
#endif

static void SetWaterMultiBoundHeight(NiGeometry* apWaterMesh) {
    if (!apWaterMesh)
        return;

    DEBUG_MSG("[ SetWaterMultiBoundHeight ] Setting AABB height");
    NiGeometryData* pGeometryData = apWaterMesh->GetModelData();
    if (!pGeometryData || !pGeometryData->GetVertices()) {
        return;
    }

    NiNode* pParent = apWaterMesh->GetParent();
    if (!pParent || !IS_MULTIBOUNDNODE(pParent)) {
        DEBUG_MSG("[ SetWaterMultiBoundHeight ] Water mesh parent is null or not a multi bound node!");
        return;
    }

    BSMultiBoundNode* pMultiBoundNode = static_cast<BSMultiBoundNode*>(pParent);
    BSMultiBound* pMultiBound = pMultiBoundNode->GetMultiBound();
    if (!pMultiBound) {
        DEBUG_MSG("[ SetWaterMultiBoundHeight ] MultiBound is null!");
        return;
    }

    BSMultiBoundAABB* pMultiBoundAABB = static_cast<BSMultiBoundAABB*>(pMultiBound->GetShape());
    if (!pMultiBoundAABB || pMultiBoundAABB->GetType() != BSMultiBoundShape::BSMB_SHAPE_AABB) {
        DEBUG_MSG("[ SetWaterMultiBoundHeight ] MultiBound AABB is null or not an AABB!");
        return;
    }

    pMultiBoundAABB->Center.z = pGeometryData->GetVertices()[0].z;
    pMultiBoundAABB->HalfExtents.z = 1.f;

    apWaterMesh->SetFixedBound(true);
    pMultiBoundNode->SetFixedBound(true);
}

void BGSTerrainChunk::AttachWaterLOD(bool abForce) {
    ThisStdCall(0x6FB170, this, abForce);
    SetWaterMultiBoundHeight(spWaterMesh);
    SetWaterMultiBoundHeight(spWaterReflectMesh);
}

void BGSTerrainChunk::InitializeShaderProperty() {
    ThisStdCall(0x6FB7E0, this);
    // Fix specular flag being applied if normal map has an alpha channel
    // LOD terrain has no specular capable shader, so game uses the default ADTS shader.
    // This breaks everything, because ADTS shaders are not compatible with LOD terrain.
    BSShaderProperty* pShaderProp = static_cast<BSShaderProperty*>(spLandMesh->GetProperty(NiProperty::SHADE));
    if (pShaderProp)
        pShaderProp->SetFlag(BSShaderProperty::BSSP_SPECULAR, false);
}

void TESObjectCELL::AddReference(TESObjectREFR* apRef, bool abOnTop) {
	// Fix for imposter references being... detached from the cell after being... attached to the cell.
    // This leads to them not being updated, as they are gone from cell's list of animated references. Genius.
    // Imposters are always attached to the *current* cell player is in (whose update always happens first), so we can safely ignore calls coming from ::AssignPersistentRefsToCell afterwards
    if (apRef) {
        NiNode* pRoot = apRef->Get3D();
        if (pRoot && pRoot->GetParent() && apRef->IsImposter()) {
            DEBUG_MSG("[ TESObjectCELL::AddReference ] Ignoring imposter reference %08X - Already attached by the player", apRef->uiFormID);
            return;
        }
    }

    ThisStdCall(0x548230, this, apRef, abOnTop);
}

namespace WaterReflectionFix {
    static bool* const bForceHighDetailReflections = (bool*)0x11C7C04;
    static float fOrgLODDrop = 0.f;
    static std::vector<BSSegmentedTriShape*> kShapesToRestore;

    static void ToggleLODRecurse(NiAVObject* apObject, bool abIgnore) {
        if (!apObject)
            return;

        if (IS_NODE(apObject)) {
            NiNode* pNode = static_cast<NiNode*>(apObject);
            for (UInt32 i = 0; i < pNode->GetArrayCount(); i++)
                ToggleLODRecurse(pNode->GetAt(i), abIgnore);
        }
        else if (IS_SEGMENTEDTRISHAPE(apObject)) {
            BSSegmentedTriShape* pSegTriShape = static_cast<BSSegmentedTriShape*>(apObject);
            pSegTriShape->bIgnoreSegments = abIgnore;
            if (abIgnore) {
                if (!pSegTriShape->pSegments[0].bVisible)
                    kShapesToRestore.push_back(pSegTriShape);

                pSegTriShape->pSegments[0].bVisible = true;
            }
        }
    }

    static void __stdcall ShowLOD() {
        if (*bForceHighDetailReflections)
            return;

        NiNode* pRoot = *(NiNode**)0x11D8690;
        fOrgLODDrop = BSShaderManager::GetLODLandDrop();
        BSShaderManager::SetLODLandDrop(0.f);
        ToggleLODRecurse(pRoot, true);

    }

    static void __stdcall HideLOD() {
        if (*bForceHighDetailReflections)
            return;

        NiNode* pRoot = *(NiNode**)0x11D8690;
        ToggleLODRecurse(pRoot, false);
        BSShaderManager::SetLODLandDrop(fOrgLODDrop);

        for (BSSegmentedTriShape* pShape : kShapesToRestore)
            pShape->pSegments[0].bVisible = false;

        kShapesToRestore.clear();
    }
}

static void UpdateLODAnimations() {
	if (kAnimatedLODObjects.empty())
		return;

    std::stack<NiPointer<NiAVObject>> kRemoveStack;
    NiUpdateData kUpdateData = NiUpdateData(*(float*)0x11C3C08, true);
    for (NiAVObject* pObject : kAnimatedLODObjects) {
        if (!pObject) {
			DEBUG_MSG("Object is null, removing from animated list");
            continue;
        }

        if (pObject->m_uiRefCount > 2)
            pObject->UpdateControllers(kUpdateData);
        else {
            DEBUG_MSG("Object %x ref count %i, adding to the removal list", pObject, pObject->m_uiRefCount);
            kRemoveStack.push(pObject);
        }
    }

    while (!kRemoveStack.empty()) {
        NiAVObject* pObject = kRemoveStack.top();
        kRemoveStack.pop();
        DEBUG_MSG("Removing object %x, ref count %i", pObject, pObject->m_uiRefCount);
        kAnimatedLODObjects.erase(std::remove(kAnimatedLODObjects.begin(), kAnimatedLODObjects.end(), pObject), kAnimatedLODObjects.end());
    }
}

static void UpdateLOD() {
	if (!bAllowForceUpdates)
		return;

    auto pWorld = TES::GetWorldSpace();
    if (!pWorld)
        return;

    BGSTerrainManager* pTerrainManager = pWorld->GetTerrainManager();
	if (!pTerrainManager)
		return;

    if (pTerrainManager->bNeedsImmediateUpdate == false)
        return;

    DEBUG_MSG("Force Updating LOD");
	PlayerCharacter* pPlayer = PlayerCharacter::GetSingleton();
    if (TESMain::GetSingleton()->bIsFlyCam)
        pTerrainManager->Update(&pPlayer->kFlycamPos.kPosition, 0xF);
    else 
        pTerrainManager->Update(pPlayer->GetPos(), 0xF);
}

static void MessageHandler(NVSEMessagingInterface::Message* msg) {
    switch (msg->type) {
    case NVSEMessagingInterface::kMessage_MainGameLoop:
        if (!TESMain::GetSingleton()->bFreezeTime) {
            UpdateLODAnimations();
            UpdateLOD();
        }
        break;
    default:
        break;
    }
}

bool NVSEPlugin_Load(NVSEInterface* nvse) {
	if (!nvse->isEditor) {
        ((NVSEMessagingInterface*)nvse->QueryInterface(kInterface_Messaging))->RegisterListener(nvse->GetPluginHandle(), "NVSE", MessageHandler);

        char iniDir[MAX_PATH];
        GetModuleFileNameA(GetModuleHandle(NULL), iniDir, MAX_PATH);
        strcpy((char*)(strrchr(iniDir, '\\') + 1), "Data\\NVSE\\Plugins\\LOD Fixes.ini");
        bUseSpecular = GetPrivateProfileInt("Main", "bUseSpecular", 0, iniDir);
        uiSpecularShine = GetPrivateProfileInt("Main", "uiSpecularShine", 65, iniDir);
		bAllowForceUpdates = GetPrivateProfileInt("Main", "bAllowForceUpdates", 1, iniDir);

		ReplaceCallEx(0x6FB0FB, &BGSTerrainChunk::InitializeShaderProperty);
        ReplaceCallEx(0x6F6011, &BGSDistantObjectBlock::Prepare);

		ReplaceCallEx(0x6F696E, &BGSTerrainNode::UpdateBlockVisibility);
		ReplaceCallEx(0x6F6C73, &BGSTerrainNode::UpdateBlockVisibility);
		ReplaceCallEx(0x6FE0F5, &BGSTerrainNode::UpdateBlockVisibility);

        // Fix imposters failing to animate
        // Only one specific call to AddReference is the offender here
        ReplaceCallEx(0x5881D1, &TESObjectCELL::AddReference);

        // Fix object LOD not reflecting in water
        ReplaceCall(0x4EB6C6, WaterReflectionFix::ShowLOD);
        ReplaceCall(0x4EB893, WaterReflectionFix::HideLOD);

        // Fix water LOD multibound height
        for (UInt32 uiAddr : {0x6FA964, 0x6FB0C0 })
            ReplaceCallEx(uiAddr, &BGSTerrainChunk::AttachWaterLOD);

#if TREELOD
		// Legacy patches, not needed anymore with latest LODGen
        // Will be removed in the future
        ReplaceCall(0x6F8621, BSMap__SetAt)
        for (UInt32 uiAddr : {0x6F9342, 0x6FCCE2, 0x6FCFDF })
            ReplaceCall(uiAddr, BGSDistantTreeBlock::HideLOD);

        // Debug
#if LOGGING
        CreateDirectory("logs", NULL);
        ReplaceCall(0x485B7D, GetFile);
        ReplaceCall(0x6FE406, BGSDistantTreeBlock__BGSDistantTreeBlock);
        SafeWrite32(0x106DED4, UInt32(BGSDistantTreeBlockLoadTask__Run));
#endif
#endif
	}

	return true;
}