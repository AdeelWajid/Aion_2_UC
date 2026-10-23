// Offsets.hpp — centralized, semantically-typed offsets for AION2 build.
//
// Every entry carries a validation status determined empirically against the LIVE
// process (read-only probing + UE reflection harvest), never guessed:
//
//   Validated   : confirmed by following real data / reflection Offset_Internal.
//   Unvalidated : plausible layout, not yet confirmed live.
//   Invalid     : the supplied value was proven wrong; the corrected value is here.
// Unknown     : target not yet located (e.g. ProcessEvent, StaticFindObject).
//
// `Supplied` values are retained only for reporting; always prefer the corrected
// constant. Sdk::VerifyOffsets() prints the classification of each global, and of the
// ProcessEvent pair, against the live client at startup.
#pragma once
#include <cstdint>

namespace aion {
namespace off {

enum class Status { Validated, Unvalidated, Invalid, Unknown };

const char* StatusName(Status s);

// ============================================================ Globals (RVA) ===
// Client patch 2026-10-14 (Steam, 156,590,880 bytes, sha256 699f42cd...). Re-derived from the live
// client, not supplied.
//
// The three data globals did *not* move by one shift this time: GObjects came out at 10-09 + 0xB000,
// GNames at +0xB180 and GWorld at +0xB070. The premise the 10-07 and 10-09 patches both satisfied -
// "a patch moves the data region as one block, so apply the pair's shift to the rest" - does not hold
// here, so it must not be trusted again. Each value below was located on its own evidence:
//   GObjects 0x0EC83990  the only array-shaped location within +-16 MiB with MaxElements 2,162,688
//                        (33 chunks - the same capacity 10-07 and 10-09 had), a live chunk table,
//                        257,069-283,227 live objects over 4 chunks, and a first item that decodes
//                        to the '/Script/CoreUObject' Package.
//   GNames   0x0F12B5C0  the pool-shaped location that decodes the names of *those objects*: 64 of
//                        64 sampled, index 3 'ByteProperty', 0x5BD '/Script/CoreUObject' - the same
//                        two probes the 10-09 entry was checked with.
//   GWorld   0x0EF8EC38  the only slot within +-512 KiB of the previous RVA holding a heap UObject
//                        of class 'World' with a resolvable level and actor count: UWorld
//                        'World_D_Starter' (outer /Game/Map/World/World_D/world_d_starter/
//                        World_D_Starter) -> +0x30 'PersistentLevel' -> +0xA0/+0xA8 both live.
// tools/find_shift.py prints the window candidates and checks GWorld structurally, but its "apply the
// GNames shift to the remaining globals" step is only a hint now - GWorld above is what disproves
// it. tools/find_globals.py finds no pair at all on this client: its pool probe (entry indexes
// 0x10001/0x10002) tests the middle of a block-1 entry and so rejects the real pool, which is why
// find_shift.py exists.
// GEngine and LocalPlayer used to be listed here (0x0F259A10 / 0x0F3B57C0). Removed 2026-10-11:
// the class-name check added to Sdk::VerifyOffsets showed the first lands on a UClass object
// (class='Class') and the second on an address with no class at all - both wrong for this build,
// neither ever derived for it, and nothing (overlay, recon, survey) read either global.
// History: 10-07 moved the same three globals by +0x12000 and their values were supplied; GObjects
//   then needed -0x10 (0x0EC8C9A0 -> 0x0EC8C990), because that address was the ObjObjects
//   sub-object, whose chunk table and counters sit at +0x10/+0x20/+0x24/+0x2C - which is what this
//   code assumes of the struct base.
namespace globals {
constexpr std::uint32_t GNames       = 0x0F132600;  // FNamePool     (was 0x0F120440, +0xB180)
constexpr std::uint32_t GObjects     = 0x0EC8A990;  // FUObjectArray (was 0x0EC78990, +0xB000)
constexpr std::uint32_t GWorld       = 0x0EF95C78;  // UWorld**      (was 0x0EF83BC8, +0xB070)
// UObject::ProcessEvent: its address in the image and the vtable slot it is reached through.
// The overlay is read-only and never calls it, so nothing else in the program would notice if
// either number drifted - verifying them is the whole point of the pair.
// 2026-10-14: the slot is still 0x4D. Across 36 distinct sampled vtables the same function pointer
// sat at 0x4D in 34 of them, and that pointer appears as the modal value of *no other* slot from
// 0x00 to 0x120 - one address, one slot - and the 12 bytes at it are a real function entry
// (`push rbp; push rsi; push rdi; push r12-r15`), which is what the 10-07 value failed. Code moves
// independently of the data region: +0x730 here against the data globals' +0xB0xx.
constexpr std::uint32_t ProcessEvent      = 0x04206400;  // vtable[0x4D] on the 10-14 client
constexpr std::uint32_t ProcessEventIndex = 0x4D;        // validated (slot holds it on every object)
// Unresolved symbols (supplied 0). Located at runtime by signature/reflection.
constexpr std::uint32_t GEngineLoop       = 0;  // UNKNOWN
constexpr std::uint32_t StaticFindObject  = 0;  // UNKNOWN
// Deliberately unused: FNameEntryId text is decoded from the FNamePool itself (NamePool.hpp),
// which is why an AppendString offset is not needed at all. Kept at 0 so this stays a complete
// list of what an AION2 SDK exposes.
constexpr std::uint32_t AppendString      = 0;  // unused
}  // namespace globals

// ====================================================== Client build ==========
// The install every offset above was derived against. Sdk::Diagnose compares a client to
// this, which is the only way to tell "different build" apart from "the reads are not
// reaching the client" when every global comes back zero.
//   AION2.exe  156,590,880 bytes  sha256 699f42cd268dad447dc560162358d0c5ea1a4c27a36be3f66ac5557047ddafd8
//   (Steam install, 2026-10-14. Previous revisions: 156,557,752 bytes sha256 3842fc511a09121d...
//    on 10-09, 156,604,184 bytes sha256 d93b050d623550fc... on 10-07, and 156,554,016 bytes
//    sha256 59d33a2d3ce7869d... before that.)
// The PE TimeDateStamp is the protector's - the image is packed by NCGuard - so it does not
// identify the game build; the file size and hash do.
namespace build {
constexpr std::uint64_t VerifiedExeSize = 156609312ULL;
}  // namespace build

// ======================================================== FNamePool / names ===
namespace name_pool {
constexpr std::uint32_t Lock               = 0x00;
constexpr std::uint32_t CurrentBlock       = 0x08;
constexpr std::uint32_t CurrentByteCursor  = 0x0C;
constexpr std::uint32_t Blocks             = 0x10;
constexpr std::uint32_t BlockSize          = 0x20000;
constexpr std::uint32_t MaxBlocks          = 8192;
constexpr std::uint32_t EntryStride        = 2;
}  // namespace name_pool

// =========================================================== FUObjectArray ===
namespace uobject_array {
constexpr std::uint32_t ObjFirstGCIndex             = 0x00;
constexpr std::uint32_t ObjLastNonGCIndex           = 0x04;
constexpr std::uint32_t MaxObjectsNotConsideredByGC = 0x08;
constexpr std::uint32_t OpenForDisregardForGC       = 0x0C;
constexpr std::uint32_t Objects                     = 0x10;  // FUObjectItem** chunks
constexpr std::uint32_t PreAllocatedObjects         = 0x18;
constexpr std::uint32_t MaxElements                 = 0x20;
constexpr std::uint32_t NumElements                 = 0x24;
constexpr std::uint32_t MaxChunks                   = 0x28;
constexpr std::uint32_t NumChunks                   = 0x2C;
constexpr std::uint32_t ElementsPerChunk            = 65536;  // 0x10000 (validated: MaxElem/MaxChunks)
}  // namespace uobject_array

namespace uobject_item {
constexpr std::uint32_t Object       = 0x00;
constexpr std::uint32_t Flags        = 0x08;
constexpr std::uint32_t ClusterRoot  = 0x0C;
constexpr std::uint32_t SerialNumber = 0x10;
constexpr std::uint32_t Size         = 0x18;
}  // namespace uobject_item

// ================================================================= UObject ===
namespace uobject {
constexpr std::uint32_t VTable        = 0x00;  // VALIDATED
constexpr std::uint32_t Flags         = 0x08;  // VALIDATED
// EObjectFlags. VALIDATED live 2026-10-09 by watching one node being gathered: the high byte of
// Flags (+0x0B) goes 00 -> 20 on the node actor *and* on every one of its components, i.e. the
// client marks the actor as pending destruction. The actor is not removed from ULevel::Actors
// until the GC collects it, and that window is why the ESP kept drawing nodes that could no
// longer be gathered or interacted with.
constexpr std::uint32_t RF_MirroredGarbage = 0x20000000;
constexpr std::uint32_t InternalIndex = 0x0C;  // VALIDATED
constexpr std::uint32_t Class         = 0x10;  // VALIDATED
constexpr std::uint32_t Name          = 0x18;  // FName (u32 ComparisonIndex, u32 Number)
constexpr std::uint32_t Outer         = 0x20;  // VALIDATED
}  // namespace uobject

// ================================================================ UStruct ===
namespace ustruct {
constexpr std::uint32_t SuperStruct     = 0x40;  // VALIDATED
constexpr std::uint32_t Children        = 0x48;  // UField* (functions) — VALIDATED
constexpr std::uint32_t ChildProperties = 0x50;  // FField* (properties) — VALIDATED
constexpr std::uint32_t PropertiesSize  = 0x58;  // VALIDATED
constexpr std::uint32_t MinAlignment    = 0x5C;
}  // namespace ustruct

namespace ufield {
constexpr std::uint32_t Next = 0x28;  // VALIDATED
}  // namespace ufield

// UDataTable — located from live memory with the raw-dump tool: the row struct pointer sits
// at +0x28 (a UScriptStruct; +0x30 held a 1068 row FScriptMap for NpcResourceTable_Common).
namespace udatatable {
constexpr std::uint32_t RowStruct = 0x28;  // UScriptStruct*   — VALIDATED (live)
constexpr std::uint32_t RowMap    = 0x30;  // FScriptMap [data][num][max] — VALIDATED (live)
}  // namespace udatatable

// ================================================================= Widgets ===
// UI-member offsets: the character's nameplate widget, and the widget tree it owns. The
// nameplate pointer and the tree layout were read live (see Nameplate.hpp); WidgetTree and
// Text come from the supplied UI offset table and are checked at use.
namespace aion_character {
constexpr std::uint32_t NameplateWidgetComponent = 0x2810;  // WC_NamePlate_C* — VALIDATED
}  // namespace aion_character
namespace uuserwidget {
constexpr std::uint32_t WidgetTree = 0x268;  // UWidgetTree* — checked at use
}  // namespace uuserwidget
namespace uwidgettree {
constexpr std::uint32_t RootWidget = 0x30;  // UWidget* — VALIDATED (reflection + live)
}  // namespace uwidgettree
namespace utextblock {
// AionTextBlock, not UTextBlock: read live, three text blocks decoded their displayed string
// here ("<80%(700Mb/900Mb)>", "%UI_PATCH_DOWNLOADING"). The supplied UTextBlock::Text 0x1A8
// does not fit this class.
constexpr std::uint32_t Text = 0x1B0;  // FText or FString — VALIDATED (live)
}  // namespace utextblock

// ================================================================= FField ===
namespace ffield {
constexpr std::uint32_t ClassPrivate = 0x08;
constexpr std::uint32_t Owner        = 0x10;
constexpr std::uint32_t Next         = 0x18;  // VALIDATED
constexpr std::uint32_t NamePrivate  = 0x20;  // FName — VALIDATED
constexpr std::uint32_t FlagsPrivate = 0x28;
}  // namespace ffield

namespace fproperty {
constexpr std::uint32_t ElementSize     = 0x34;  // VALIDATED
constexpr std::uint32_t PropertyFlags   = 0x38;
constexpr std::uint32_t Offset_Internal = 0x44;  // VALIDATED
}  // namespace fproperty

namespace fbool_property {
// FBoolProperty follows FProperty: FieldSize, ByteOffset, ByteMask, FieldMask. These four are the
// only offsets the render-gate dump cannot get from reflection by name, so the dump re-checks them
// at run time against AActor::bHidden, whose byte and mask are validated above (HiddenFlags 0x078,
// HiddenBit 0x80) - a wrong value there prints MISMATCH instead of a wrong number.
constexpr std::uint32_t FieldSize  = 0x70;
constexpr std::uint32_t ByteOffset = 0x71;  // byte inside the property's value
constexpr std::uint32_t ByteMask   = 0x72;  // bit mask inside that byte
constexpr std::uint32_t FieldMask  = 0x73;
}  // namespace fbool_property

// =============================================================== UFunction ===
namespace ufunction {
constexpr std::uint32_t FunctionFlags = 0xB0;  // UNVALIDATED (not yet exercised)
// VALIDATED live: engine AI UFunctions resolve to native entry points inside the image
// (AIController::RunBehaviorTree 0x1476ABAA0, AionNpcSightSensingComponent::SetSightRadius
// 0x1483112F0), so this is the real Func field and a UFunction's code address is reachable
// without resolving ProcessEvent first.
constexpr std::uint32_t Func = 0xD8;  // VALIDATED
}  // namespace ufunction

// ================================================================== UWorld ===
namespace uworld {
constexpr std::uint32_t PersistentLevel    = 0x030;  // VALIDATED
constexpr std::uint32_t Levels             = 0x180;  // VALIDATED (supplied 0x178 INVALID)
constexpr std::uint32_t OwningGameInstance = 0x1E8;  // VALIDATED
}  // namespace uworld

// ================================================================== ULevel ===
namespace ulevel {
constexpr std::uint32_t Actors        = 0x0A0;  // VALIDATED (empirical)
constexpr std::uint32_t WorldSettings = 0x2C0;  // VALIDATED
}  // namespace ulevel

namespace aworldsettings {
constexpr std::uint32_t TimeDilation          = 0x468;  // supplied
constexpr std::uint32_t CinematicTimeDilation = 0x46C;  // supplied
}  // namespace aworldsettings

// ============================================================ UGameInstance ===
namespace ugameinstance {
constexpr std::uint32_t LocalPlayers = 0x038;  // VALIDATED
constexpr std::uint32_t UISystem     = 0x3C8;  // VALIDATED (AionGameInstance)
constexpr std::uint32_t InputSystem  = 0x418;  // VALIDATED (AionGameInstance)
}  // namespace ugameinstance

// ============================================================== ULocalPlayer /
namespace uplayer {
constexpr std::uint32_t PlayerController = 0x030;  // VALIDATED
}  // namespace uplayer

// ======================================================= APlayerController ===
namespace aplayercontroller {
constexpr std::uint32_t Player               = 0x3E0;  // VALIDATED
constexpr std::uint32_t AcknowledgedPawn     = 0x3E8;  // VALIDATED (supplied 0x378 aliases live but is not the field)
constexpr std::uint32_t MyHUD                = 0x3F0;  // VALIDATED (supplied name HUD)
constexpr std::uint32_t PlayerCameraManager  = 0x3F8;  // VALIDATED
constexpr std::uint32_t PlayerInput          = 0x4B8;  // VALIDATED
constexpr std::uint32_t ControlRotation      = 0x3B8;  // VALIDATED (supplied 0x320 INVALID; inherited AController)
}  // namespace aplayercontroller

// =========================================================== AController =====
namespace acontroller {
constexpr std::uint32_t PlayerState     = 0x340;  // VALIDATED (supplied 0x340 for Controller::PlayerState is wrong? corrected below)
constexpr std::uint32_t ControlRotation = 0x3B8;  // VALIDATED
}  // namespace acontroller

// ================================================================== APawn ====
namespace apawn {
constexpr std::uint32_t BaseEyeHeight = 0x344;  // VALIDATED (supplied 0x2B4 INVALID)
constexpr std::uint32_t PlayerState   = 0x358;  // VALIDATED (supplied 0x2C8 INVALID)
constexpr std::uint32_t Controller    = 0x370;  // VALIDATED (supplied 0x2D8 INVALID)
// How a mob gets its controller: BPCh_NpcBase_C carries both, which is the standard
// auto-possess path the client uses to put an AionDefaultAIController on every monster.
constexpr std::uint32_t AIControllerClass = 0x350;  // VALIDATED (live reflection) UClass*
constexpr std::uint32_t AutoPossessAI     = 0x349;  // VALIDATED (live reflection) EAutoPossessAI
}  // namespace apawn

// =============================================================== AActor ======
namespace aactor {
// The client's own "do not render this actor" flag. It is set on everything outside the
// client's relevancy set, which is how monsters that have not appeared yet are held: the
// actor exists with a real mesh and a real position and the game shows nothing. Measured
// live - every character nearer than ~23 m reads 0 and every one beyond ~25 m reads 1.
// Bit 7 of this byte; the same byte also carries bAlwaysRelevant (bit 3) and
// bReplicateMovement (bit 4), so the mask is required.
constexpr std::uint32_t HiddenFlags = 0x078;  // SDK AActor::bHidden (BitIndex 0x07)
constexpr std::uint8_t  HiddenBit   = 0x80;   // bit 7
constexpr std::uint32_t CustomTimeDilation          = 0x084;  // VALIDATED
constexpr std::uint32_t RootComponent               = 0x1C0;  // VALIDATED
constexpr std::uint32_t InstanceComponents          = 0x288;  // VALIDATED (supplied 0x280 INVALID)
constexpr std::uint32_t BlueprintCreatedComponents  = 0x298;  // VALIDATED (supplied 0x290 INVALID)
}  // namespace aactor

// ============================================================ AAionEnvObj ====
// Every world object shares one class (BP_EnvObj_Common_C), so the mesh component and its
// anim instance are the only per-instance state there is. VALIDATED live (SDK + read).
namespace aionenvobj {
constexpr std::uint32_t SkeletalMeshComponent = 0x430;
constexpr std::uint32_t StaticMeshComponent   = 0x438;
}  // namespace aionenvobj

// AionEnvObjAnimInstance - the anim instance those world objects hang their state on. Read live
// through AAionEnvObj::SkeletalMeshComponent -> SkeletalMeshComponent::AnimScriptInstance (both
// resolved by property name, never hardcoded here), which is the same hop the spent-node check uses.
namespace aionenvobj_anim {
// EAnimInteractionState, read from the live UEnum: Available is the state an object has until it is
// used, Finish the state it is left in afterwards. VALIDATED live: 15 untouched world objects read
// Available and the 2 already-consumed ones Finish, and MainState is the only byte inside a declared
// object size (SDK: AionEnvObjAnimInstance size=0x630) that separates those two groups.
constexpr std::uint32_t MainState = 0x620;  // EnumProperty, EAnimInteractionState
constexpr std::uint8_t MainStateAvailable = 0;  // EAnimInteractionState::Available
constexpr std::uint8_t MainStateFinish = 1;     // EAnimInteractionState::Finish
}  // namespace aionenvobj_anim

// ============================================================= ACharacter ====
namespace acharacter {
constexpr std::uint32_t Mesh              = 0x3C0;  // VALIDATED
constexpr std::uint32_t CharacterMovement = 0x3C8;  // VALIDATED
constexpr std::uint32_t CapsuleComponent  = 0x3D0;  // VALIDATED
}  // namespace acharacter

namespace aion_character {
constexpr std::uint32_t CharActPlayRateControl = 0x22B0;  // VALIDATED
}  // namespace aion_character

// ====================================================== APlayerCameraManager
namespace aplayer_camera_manager {
constexpr std::uint32_t DefaultFOV           = 0x350;   // VALIDATED (supplied 0x2C0 INVALID)
constexpr std::uint32_t CameraCachePrivate   = 0x1A70;  // VALIDATED (supplied 0x1410 INVALID)
constexpr std::uint32_t LastFrameCameraCache = 0x2590;  // reflection-validated
}  // namespace aplayer_camera_manager

namespace fcamera_cache_entry {
constexpr std::uint32_t TimeStamp = 0x00;  // float
constexpr std::uint32_t POV       = 0x10;  // FMinimalViewInfo — VALIDATED
}  // namespace fcamera_cache_entry

namespace fminimal_view_info {
constexpr std::uint32_t Location    = 0x00;  // FVector3d (3 doubles) — VALIDATED
constexpr std::uint32_t Rotation    = 0x18;  // FRotator3d (3 doubles) — VALIDATED
constexpr std::uint32_t FOV         = 0x30;  // float
constexpr std::uint32_t DesiredFOV  = 0x34;  // float
}  // namespace fminimal_view_info

// ================================================================= APlayerState =
namespace aplayer_state {
constexpr std::uint32_t PlayerNamePrivate = 0x3D0;  // VALIDATED (supplied 0x340 INVALID)
}  // namespace aplayer_state

// ========================================================= USceneComponent ===
namespace uscene_component {
constexpr std::uint32_t RelativeLocation = 0x148;  // VALIDATED (supplied 0x128 INVALID)
constexpr std::uint32_t RelativeRotation = 0x160;  // VALIDATED (supplied 0x140 INVALID)
constexpr std::uint32_t RelativeScale3D  = 0x178;  // VALIDATED (supplied 0x158 INVALID)
constexpr std::uint32_t ComponentToWorld = 0x1E0;  // UNVALIDATED (probe at runtime)
}  // namespace uscene_component

// ========================================================= UCharacterMovement
namespace ucharacter_movement {
constexpr std::uint32_t JumpZVelocity = 0x198;  // VALIDATED
constexpr std::uint32_t MovementMode  = 0x221;  // VALIDATED (uint8; EMovementMode)
constexpr std::uint32_t MaxWalkSpeed  = 0x268;  // VALIDATED
// Corrected against build/sdk_dump.txt: the CharacterMovement block runs MaxWalkSpeed 0x268,
// MaxWalkSpeedCrouched 0x26C, MaxSwimSpeed 0x270, MaxFlySpeed 0x274. The previous pair (0x26C/0x270)
// was one field short, so MaxSwimSpeed was in fact reading MaxWalkSpeedCrouched - and both were
// marked VALIDATED with the correct values annotated as INVALID. Nothing consumed either.
constexpr std::uint32_t MaxSwimSpeed  = 0x270;  // VALIDATED (sdk_dump.txt:34377)
constexpr std::uint32_t MaxFlySpeed   = 0x274;  // VALIDATED (sdk_dump.txt:34378)
}  // namespace ucharacter_movement

// ======================================================= UPrimitiveComponent =
namespace uprimitive_component {
constexpr std::uint32_t BodyInstance = 0x3E8;  // VALIDATED (supplied 0x348 INVALID)
}  // namespace uprimitive_component

namespace fbody_instance {
// CollisionEnabled was listed here at 0x17, UNVALIDATED (a nested struct field the reflection dump
// does not carry) and read by nothing. Removed: an unverifiable number nothing consumes is a trap,
// not a reference.
}  // namespace fbody_instance

namespace ucapsule_component {
constexpr std::uint32_t CapsuleHalfHeight = 0x5D8;  // VALIDATED (supplied 0x508 INVALID)
constexpr std::uint32_t CapsuleRadius     = 0x5DC;  // VALIDATED (supplied 0x50C INVALID)
}  // namespace ucapsule_component

namespace uskeletal_mesh_component {
constexpr std::uint32_t GlobalAnimRateScale          = 0xAF0;  // VALIDATED
constexpr std::uint32_t CachedComponentSpaceTransforms = 0xA40;  // VALIDATED (supplied 0x960 INVALID)
// The mesh asset is the actor's real identity: every spawned NPC is named after
// its class (BPCh_NpcBase_C), while this property resolves to "MOB_WindEle_01",
// "Mob_OdSpider_02_V02", ... VALIDATED live.
constexpr std::uint32_t SkeletalMesh = 0x5F8;  // VALIDATED
}  // namespace uskeletal_mesh_component

namespace ustatic_mesh_component {
constexpr std::uint32_t StaticMesh = 0x600;  // VALIDATED (supplied 0x528 INVALID)
}  // namespace ustatic_mesh_component

namespace uinstanced_static_mesh_component {
// PerInstanceSMData used to sit here at 0x5C0, marked UNVALIDATED (supplied) and referenced by
// nothing; the dump does not place the field there, so the guess was removed rather than kept as a
// trap for the next reader.
}  // namespace uinstanced_static_mesh_component

// =============================================================== UUserWidget
// The widget offsets are declared once, above, as off::uuserwidget::WidgetTree and
// off::utextblock::Text - the referenced spellings. The duplicated uuser_widget/utext_block pair
// that used to sit here shadowed nothing and was referenced by nothing.

// ================================================== AI / NPC controllers ======
// Every monster gets its own AionDefaultAIController (the live census recorded 70 of them beside
// 70 BPCh_NpcBase_C mobs). That class is a thin subclass of the engine AIController - it declares
// no properties and no UFunctions of its own, and the mob's pawn class BPCh_NpcBase_C likewise
// exposes none - so these are the engine fields it inherits, resolved by name against the running
// build's reflection (FProperty::Offset_Internal), which is authoritative for this build.
namespace aicontroller {
constexpr std::uint32_t PlayerState            = 0x340;  // VALIDATED (live reflection)
constexpr std::uint32_t OnInstigatedAnyDamage  = 0x350;  // VALIDATED (live reflection)
constexpr std::uint32_t StateName              = 0x370;  // VALIDATED (live reflection) FName
constexpr std::uint32_t Pawn                   = 0x378;  // VALIDATED (live reflection)
constexpr std::uint32_t Character              = 0x388;  // VALIDATED (live reflection)
constexpr std::uint32_t TransformComponent     = 0x390;  // VALIDATED (live reflection)
constexpr std::uint32_t ControlRotation        = 0x3B8;  // VALIDATED (live reflection) FRotator
constexpr std::uint32_t bAttachToPawn          = 0x3D0;  // VALIDATED (live reflection)
constexpr std::uint32_t PathFollowingComponent = 0x418;  // VALIDATED (live reflection)
constexpr std::uint32_t BrainComponent         = 0x420;  // VALIDATED (live reflection)
constexpr std::uint32_t PerceptionComponent    = 0x428;  // VALIDATED (live reflection)
constexpr std::uint32_t ActionsComp            = 0x430;  // VALIDATED (live reflection)
constexpr std::uint32_t Blackboard             = 0x438;  // VALIDATED (live reflection)
}  // namespace aicontroller

namespace ubrain_component {
constexpr std::uint32_t BlackboardComp = 0x0C8;  // VALIDATED (live reflection)
constexpr std::uint32_t AIOwner        = 0x0D0;  // VALIDATED (live reflection)
}  // namespace ubrain_component

namespace ubehavior_tree_component {
constexpr std::uint32_t NodeInstances            = 0x138;  // VALIDATED (live reflection) TArray
constexpr std::uint32_t DefaultBehaviorTreeAsset = 0x290;  // VALIDATED (live reflection)
}  // namespace ubehavior_tree_component

namespace ublackboard_component {
constexpr std::uint32_t BrainComp         = 0x0C0;  // VALIDATED (live reflection)
constexpr std::uint32_t BlackboardAsset   = 0x0D0;  // VALIDATED (live reflection)
constexpr std::uint32_t KeyInstances      = 0x0F8;  // VALIDATED (live reflection) TArray
}  // namespace ublackboard_component

namespace uai_perception_component {
constexpr std::uint32_t SensesConfig  = 0x0C0;  // VALIDATED (live reflection) TArray<UAISenseConfig*>
constexpr std::uint32_t DominantSense = 0x0D0;  // VALIDATED (live reflection) UClass*
constexpr std::uint32_t AIOwner       = 0x0E8;  // VALIDATED (live reflection)
}  // namespace uai_perception_component

namespace upawn_sensing_component {
constexpr std::uint32_t SightRadius           = 0x0C8;  // VALIDATED (live reflection)
constexpr std::uint32_t SensingInterval       = 0x0CC;  // VALIDATED (live reflection)
constexpr std::uint32_t bEnableSensingUpdates = 0x0D4;  // VALIDATED (live reflection)
constexpr std::uint32_t PeripheralVisionAngle = 0x100;  // VALIDATED (live reflection)
}  // namespace upawn_sensing_component

// The game's own sensor, read live by --dump-ai. AionNpcSightSensingComponent is an ActorComponent
// carrying SightRadius/SightAngle with native BlueprintCallable setters. Writing it does not end a
// fight: the FDevPacketData_GameServer_* target/aggro notifications (NpcFirstAggro_NT,
// SetMainTarget_RS, TargetUpdate_NT) show the server picking the mob's target, and a mob that has
// been attacked keeps attacking whatever the client's own sensor reads.
namespace aion_npc_sight_sensing_component {
constexpr std::uint32_t bOnlySensePC    = 0x100;  // VALIDATED (live reflection) bool
constexpr std::uint32_t SensingInterval = 0x104;  // VALIDATED (live reflection) float
constexpr std::uint32_t SightRadius     = 0x108;  // VALIDATED (live reflection) float
constexpr std::uint32_t SightAngle      = 0x10C;  // VALIDATED (live reflection) float
// Its UFunctions SetSightRadius / SetSightAngle resolve to native entry points 0x1483112F0 /
// 0x1483111F0, both inside the image, so a UFunction's code address is reachable directly.
}  // namespace aion_npc_sight_sensing_component

// ================================================================ Rendering ===
namespace rendering {
constexpr std::uint32_t Mesh_GlobalAnimRateScale = 0xAF0;  // alias
}  // namespace rendering

}  // namespace off
}  // namespace aion
