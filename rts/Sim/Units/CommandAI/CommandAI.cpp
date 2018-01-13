/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */


#include "CommandAI.h"

#include "BuilderCAI.h"
#include "FactoryCAI.h"
#include "ExternalAI/EngineOutHandler.h"
#include "ExternalAI/SkirmishAIHandler.h"
#include "Game/GlobalUnsynced.h"
#include "Game/SelectedUnitsHandler.h"
#include "Game/WaitCommandsAI.h"
#include "Map/Ground.h"
#include "Map/MapDamage.h"
#include "Sim/Features/Feature.h"
#include "Sim/Features/FeatureDef.h"
#include "Sim/Features/FeatureHandler.h"
#include "Sim/Misc/GlobalSynced.h"
#include "Sim/Misc/ModInfo.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/MoveTypes/MoveType.h"
#include "Sim/Units/BuildInfo.h"
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitHandler.h"
#include "Sim/Weapons/Weapon.h"
#include "Sim/Weapons/WeaponDef.h"
#include "System/EventHandler.h"
#include "System/myMath.h"
#include "System/Log/ILog.h"
#include "System/SafeUtil.h"
#include "System/StringUtil.h"
#include "System/creg/STL_Set.h"
#include "System/creg/STL_Deque.h"
#include <assert.h>

// number of SlowUpdate calls that a target (unit) must
// be out of radar (and hence LOS) contact before it is
// considered 'lost' and invalid (for attack orders etc)
//
// historically this value was 120, which meant that it
// took (120 * UNIT_SLOWUPDATE_RATE) / GAME_SPEED == 64
// seconds (!) before eg. aircraft would stop tracking a
// target that cloaked after flying over it --> obviously
// unreasonable
static const int TARGET_LOST_TIMER = 4;
static const float COMMAND_CANCEL_DIST = 17.0f;

void CCommandAI::InitCommandDescriptionCache() { commandDescriptionCache = new CCommandDescriptionCache(); }
void CCommandAI::KillCommandDescriptionCache() { spring::SafeDelete(commandDescriptionCache); }

CR_BIND(CCommandQueue, )
CR_REG_METADATA(CCommandQueue, (
	CR_MEMBER(queue),
	CR_MEMBER(queueType),
	CR_MEMBER(tagCounter)
))

CR_BIND_DERIVED(CCommandAI, CObject, )
CR_REG_METADATA(CCommandAI, (
	CR_MEMBER(stockpileWeapon),

	CR_MEMBER(possibleCommands),
	CR_MEMBER(nonQueingCommands),
	CR_MEMBER(commandQue),
	CR_MEMBER(lastUserCommand),
	CR_MEMBER(selfDCountdown),
	CR_MEMBER(lastFinishCommand),

	CR_MEMBER(owner),

	CR_MEMBER(orderTarget),
	CR_MEMBER(targetDied),
	CR_MEMBER(inCommand),
	CR_MEMBER(repeatOrders),
	CR_MEMBER(lastSelectedCommandPage),
	CR_MEMBER(commandDeathDependences),
	CR_MEMBER(targetLostTimer)
))

CCommandAI::CCommandAI():
	stockpileWeapon(0),
	lastUserCommand(-1000),
	selfDCountdown(0),
	lastFinishCommand(0),
	owner(NULL),
	orderTarget(0),
	targetDied(false),
	inCommand(false),
	repeatOrders(false),
	lastSelectedCommandPage(0),
	targetLostTimer(TARGET_LOST_TIMER)
{}

CCommandAI::CCommandAI(CUnit* owner):
	stockpileWeapon(0),
	lastUserCommand(-1000),
	selfDCountdown(0),
	lastFinishCommand(0),
	owner(owner),
	orderTarget(0),
	targetDied(false),
	inCommand(false),
	repeatOrders(false),
	lastSelectedCommandPage(0),
	targetLostTimer(TARGET_LOST_TIMER)
{
	{
		SCommandDescription c;

		c.id   = CMD_STOP;
		c.type = CMDTYPE_ICON;

		c.action    = "stop";
		c.name      = "Stop";
		c.tooltip   = c.name + ": Cancel the units current actions";
		c.mouseicon = c.name;
		possibleCommands.push_back(commandDescriptionCache->GetPtr(c));
	}

	if (IsAttackCapable()) {
		SCommandDescription c;

		c.id   = CMD_ATTACK;
		c.type = CMDTYPE_ICON_UNIT_OR_MAP;

		c.action    = "attack";
		c.name      = "Attack";
		c.tooltip   = c.name + ": Attacks a unit or a position on the ground";
		c.mouseicon = c.name;
		possibleCommands.push_back(commandDescriptionCache->GetPtr(c));
	}

	if (owner->unitDef->canManualFire) {
		SCommandDescription c;

		c.id   = CMD_MANUALFIRE;
		c.type = CMDTYPE_ICON_MAP;

		c.action    = "manualfire";
		c.name      = "ManualFire";
		c.tooltip   = c.name + ": Attacks with manually-fired weapon";
		c.mouseicon = c.name;
		possibleCommands.push_back(commandDescriptionCache->GetPtr(c));
	}

	{
		SCommandDescription c;

		c.id   = CMD_WAIT;
		c.type = CMDTYPE_ICON;

		c.action    = "wait";
		c.name      = "Wait";
		c.tooltip   = c.name + ": Tells the unit to wait processing its command-queue";
		c.mouseicon = c.name;
		possibleCommands.push_back(commandDescriptionCache->GetPtr(c));
	}

	{
		SCommandDescription c;

		c.id   = CMD_TIMEWAIT;
		c.type = CMDTYPE_NUMBER;

		c.action    = "timewait";
		c.name      = "TimeWait";
		c.tooltip   = c.name + ": Wait for a period of time before continuing";
		c.mouseicon = c.name;

		c.params.push_back("1");  // min
		c.params.push_back("60"); // max

		c.hidden = true;
		possibleCommands.push_back(commandDescriptionCache->GetPtr(c));
	}

	{
		SCommandDescription c;

		// only for games with 2 ally teams  --  checked later
		c.id   = CMD_DEATHWAIT;
		c.type = CMDTYPE_ICON_UNIT_OR_RECTANGLE;

		c.action    = "deathwait";
		c.name      = "DeathWait";
		c.tooltip   = c.name + ": Wait until units die before continuing";
		c.mouseicon = c.name;

		c.hidden = true;
		possibleCommands.push_back(commandDescriptionCache->GetPtr(c));
	}

	{
		SCommandDescription c;

		c.id   = CMD_SQUADWAIT;
		c.type = CMDTYPE_NUMBER;

		c.action    = "squadwait";
		c.name      = "SquadWait";
		c.tooltip   = c.name + ": Wait for a number of units to arrive before continuing";
		c.mouseicon = c.name;

		c.params.push_back("2");   // min
		c.params.push_back("100"); // max

		c.hidden = true;
		possibleCommands.push_back(commandDescriptionCache->GetPtr(c));
	}

	{
		SCommandDescription c;

		c.id   = CMD_GATHERWAIT;
		c.type = CMDTYPE_ICON;

		c.action    = "gatherwait";
		c.name      = "GatherWait";
		c.tooltip   = c.name + ": Wait until all units arrive before continuing";
		c.mouseicon = c.name;

		c.hidden = true;
		possibleCommands.push_back(commandDescriptionCache->GetPtr(c));
	}

	if (owner->unitDef->canSelfD) {
		SCommandDescription c;

		c.id   = CMD_SELFD;
		c.type = CMDTYPE_ICON;

		c.action    = "selfd";
		c.name      = "SelfD";
		c.tooltip   = c.name + ": Tells the unit to self destruct";
		c.mouseicon = c.name;

		c.hidden = true;
		possibleCommands.push_back(commandDescriptionCache->GetPtr(c));
	}

	if (CanChangeFireState()) {
		SCommandDescription c;

		c.id   = CMD_FIRE_STATE;
		c.type = CMDTYPE_ICON_MODE;

		c.action    = "firestate";
		c.name      = "Fire state";
		c.tooltip   = c.name + ": Sets under what conditions an\nunit will start to fire at enemy units\nwithout an explicit attack order";
		c.mouseicon = c.name;

		c.params.push_back(IntToString(FIRESTATE_FIREATWILL));
		c.params.push_back("Hold fire");
		c.params.push_back("Return fire");
		c.params.push_back("Fire at will");

		c.hidden   = false;
		c.queueing = false;
		possibleCommands.push_back(commandDescriptionCache->GetPtr(c));
	}

	if (owner->unitDef->canmove || owner->unitDef->builder) {
		SCommandDescription c;

		c.id   = CMD_MOVE_STATE;
		c.type = CMDTYPE_ICON_MODE;

		c.action    = "movestate";
		c.name      = "Move state";
		c.tooltip   = c.name + ": Sets how far out of its way\nan unit will move to attack enemies";
		c.mouseicon = c.name;

		c.params.push_back(IntToString(MOVESTATE_MANEUVER));
		c.params.push_back("Hold pos");
		c.params.push_back("Maneuver");
		c.params.push_back("Roam");

		c.hidden   = false;
		c.queueing = false;
		possibleCommands.push_back(commandDescriptionCache->GetPtr(c));
	} else {
		owner->moveState = MOVESTATE_HOLDPOS;
	}

	if (owner->unitDef->canRepeat) {
		SCommandDescription c;

		c.id   = CMD_REPEAT;
		c.type = CMDTYPE_ICON_MODE;

		c.action    = "repeat";
		c.name      = "Repeat";
		c.tooltip   = c.name + ": If on, the unit will continuously\npush finished orders to the end of its\norder queue";
		c.mouseicon = c.name;

		c.params.push_back("0");
		c.params.push_back("Repeat off");
		c.params.push_back("Repeat on");

		c.hidden   = false;
		c.queueing = false;
		possibleCommands.push_back(commandDescriptionCache->GetPtr(c));
	}

	if (owner->unitDef->highTrajectoryType > 1) {
		SCommandDescription c;

		c.id   = CMD_TRAJECTORY;
		c.type = CMDTYPE_ICON_MODE;

		c.action    = "trajectory";
		c.name      = "Trajectory";
		c.tooltip   = c.name + ": If set to high, weapons that\nsupport it will try to fire in a higher\ntrajectory than usual (experimental)";
		c.mouseicon = c.name;

		c.params.push_back("0");
		c.params.push_back("Low traj");
		c.params.push_back("High traj");

		c.hidden   = false;
		c.queueing = false;
		possibleCommands.push_back(commandDescriptionCache->GetPtr(c));
	}

	if (owner->unitDef->onoffable) {
		SCommandDescription c;

		c.id   = CMD_ONOFF;
		c.type = CMDTYPE_ICON_MODE;

		c.action    = "onoff";
		c.name      = "Active state";
		c.tooltip   = c.name + ": Sets the active state of the unit to on or off";
		c.mouseicon = c.name;

		c.params.push_back(IntToString(owner->unitDef->activateWhenBuilt, "%d"));
		c.params.push_back(" Off ");
		c.params.push_back(" On ");

		c.hidden   = false;
		c.queueing = false;
		possibleCommands.push_back(commandDescriptionCache->GetPtr(c));
	}

	if (owner->unitDef->canCloak) {
		SCommandDescription c;

		c.id   = CMD_CLOAK;
		c.type = CMDTYPE_ICON_MODE;

		c.action    = "cloak";
		c.name      = "Cloak state";
		c.tooltip   = c.name + ": Sets whether the unit is cloaked or not";
		c.mouseicon = c.name;

		c.params.push_back(IntToString(owner->unitDef->startCloaked, "%d"));
		c.params.push_back("UnCloaked");
		c.params.push_back("Cloaked");

		c.hidden   = false;
		c.queueing = false;
		possibleCommands.push_back(commandDescriptionCache->GetPtr(c));
	}

	UpdateNonQueueingCommands();
}

CCommandAI::~CCommandAI()
{
	SetOrderTarget(NULL);
	ClearCommandDependencies();
	commandDescriptionCache->DecRef(possibleCommands);
}


void CCommandAI::UpdateCommandDescription(unsigned int cmdDescIdx, const Command& cmd) {
	SCommandDescription cd = *possibleCommands[cmdDescIdx];
	cd.params[0] = IntToString(int(cmd.GetParam(0)), "%d");
	commandDescriptionCache->DecRef(*possibleCommands[cmdDescIdx]);
	possibleCommands[cmdDescIdx] = commandDescriptionCache->GetPtr(cd);
}

void CCommandAI::UpdateCommandDescription(unsigned int cmdDescIdx, const SCommandDescription& modCmdDesc) {
	const SCommandDescription* curCmdDesc = possibleCommands[cmdDescIdx];

	// modCmdDesc should be a modified copy of curCmdDesc
	assert(&modCmdDesc != curCmdDesc);

	// erase in case we do not want it to be non-queueing anymore
	if (!curCmdDesc->queueing)
		nonQueingCommands.erase(curCmdDesc->id);

	// re-insert otherwise (possibly with a different cmdID!)
	if (!modCmdDesc.queueing)
		nonQueingCommands.insert(modCmdDesc.id);
	commandDescriptionCache->DecRef(*curCmdDesc);

	// update
	possibleCommands[cmdDescIdx] = commandDescriptionCache->GetPtr(modCmdDesc);

	selectedUnitsHandler.PossibleCommandChange(owner);
}

void CCommandAI::InsertCommandDescription(unsigned int cmdDescIdx, const SCommandDescription& cmdDesc)
{
	const SCommandDescription* cmdDescPtr = commandDescriptionCache->GetPtr(cmdDesc);
	if (cmdDescIdx >= possibleCommands.size()) {
		possibleCommands.push_back(cmdDescPtr);
	} else {
		// preserve order
		possibleCommands.insert(possibleCommands.begin() + cmdDescIdx, cmdDescPtr);
	}

	if (!cmdDesc.queueing)
		nonQueingCommands.insert(cmdDesc.id);

	selectedUnitsHandler.PossibleCommandChange(owner);
}

bool CCommandAI::RemoveCommandDescription(unsigned int cmdDescIdx)
{
	if (cmdDescIdx >= possibleCommands.size())
		return false;

	if (!possibleCommands[cmdDescIdx]->queueing)
		nonQueingCommands.erase(possibleCommands[cmdDescIdx]->id);

	commandDescriptionCache->DecRef(*possibleCommands[cmdDescIdx]);
	// preserve order
	possibleCommands.erase(possibleCommands.begin() + cmdDescIdx);
	selectedUnitsHandler.PossibleCommandChange(owner);
	return true;
}


void CCommandAI::UpdateNonQueueingCommands()
{
	nonQueingCommands.clear();

	for (const SCommandDescription* cmdDesc: possibleCommands) {
		if (!cmdDesc->queueing) {
			nonQueingCommands.insert(cmdDesc->id);
		}
	}
}


void CCommandAI::ClearCommandDependencies() {
	while (!commandDeathDependences.empty()) {
		DeleteDeathDependence(*commandDeathDependences.begin(), DEPENDENCE_COMMANDQUE);
	}
}

void CCommandAI::AddCommandDependency(const Command& c) {
	int cpos;

	if (!c.IsObjectCommand(cpos))
		return;

	const int refId = c.params[cpos];

	CObject* ref = (refId < unitHandler->MaxUnits()) ?
		static_cast<CObject*>(unitHandler->GetUnit(refId)) :
		static_cast<CObject*>(featureHandler->GetFeature(refId - unitHandler->MaxUnits()));

	if (ref == nullptr)
		return;

	AddDeathDependence(ref, DEPENDENCE_COMMANDQUE);
}


bool CCommandAI::IsAttackCapable() const
{
	const UnitDef* ud = owner->unitDef;
	const bool b = (!ud->weapons.empty() || ud->canKamikaze || (ud->IsFactoryUnit()));

	return (ud->canAttack && b);
}



static inline const CUnit* GetCommandUnit(const Command& c, int idx) {
	if (idx >= c.params.size())
		return nullptr;

	if (c.IsAreaCommand())
		return nullptr;

	return (unitHandler->GetUnit(c.params[idx]));
}

static inline bool IsCommandInMap(const Command& c)
{
	if (c.params.size() < 3)
		return true;

	float3 pos;

	// TODO: other commands for which pos is not stored in params[0..2]?
	switch (c.GetID()) {
		case CMD_CAPTURE: { pos = c.GetPos(1 * (c.params.size() == 5)); } break;
		case CMD_RECLAIM: { pos = c.GetPos(1 * (c.params.size() == 5)); } break;
		case CMD_REPAIR : { pos = c.GetPos(1 * (c.params.size() == 5)); } break;
		default         : { pos = c.GetPos(0                         ); } break;
	}

	if (pos.IsInBounds())
		return true;

	LOG_L(L_DEBUG, "[%s] dropped command %d (x:%f y:%f z:%f)", __func__, c.GetID(), pos.x, pos.y, pos.z);
	return false;
}

static inline bool AdjustGroundAttackCommand(const Command& c, bool fromSynced, bool aiOrder)
{
	if (c.params.size() < 3)
		return false;
	if (aiOrder)
		return false;

	const float3 cPos = c.GetPos(0);

	#if 0
	// check if attack-ground is really attack-ground
	//
	// NOTE:
	//     problematic if command contains value from UHM
	//     but is evaluated in synced context against SHM
	//     after roundtrip (when UHM and SHM differ a lot)
	//
	//     instead just clamp the elevation, which creates
	//     fewer issues overall (eg. artillery force-firing
	//     at positions outside LOS where UHM and SHM do not
	//     match will not be broken)
	//
	if (math::fabs(cPos.y - gHeight) > SQUARE_SIZE) {
		return false;
	}
	#else
	// FIXME: is fromSynced really sync-safe???
	// NOTE:
	//   uses gHeight = min(cPos.y, GetHeightAboveWater) instead
	//   of gHeight = GetHeightReal because GuiTraceRay can stop
	//   at water surface, so the attack-position would be moved
	//   UNDERWATER and cause ground-attack orders to fail (this
	//   SHOULD no longer happen, Weapon::AdjustTargetPosToWater
	//   always forcibly adjusts positions to respect waterWeapon
	//   now)
	//
	Command& cc = const_cast<Command&>(c);

	cc.params[1] = std::min(cPos.y, CGround::GetHeightAboveWater(cPos.x, cPos.z, true || fromSynced));
	// cc.params[1] = CGround::GetHeightReal(cPos.x, cPos.z, true || fromSynced);

	return true;
	#endif
}



bool CCommandAI::AllowedCommand(const Command& c, bool fromSynced)
{
	const int cmdID = c.GetID();

	// TODO check if the command is in the map first, for more commands
	switch (cmdID) {
		case CMD_MOVE:
		case CMD_ATTACK:
		case CMD_AREA_ATTACK:
		case CMD_RECLAIM:
		case CMD_REPAIR:
		case CMD_RESURRECT:
		case CMD_PATROL:
		case CMD_RESTORE:
		case CMD_FIGHT:
		case CMD_MANUALFIRE:
		case CMD_UNLOAD_UNIT:
		case CMD_UNLOAD_UNITS: {
			if (!IsCommandInMap(c))
				return false;

		} break;

		default: {
			// build commands
			if (cmdID < 0) {
				if (!IsCommandInMap(c))
					return false;

				const CBuilderCAI* bcai = dynamic_cast<const CBuilderCAI*>(this);
				const CFactoryCAI* fcai = dynamic_cast<const CFactoryCAI*>(this);

				// non-builders cannot ever execute these
				// we can get here if a factory is selected along with the
				// unit it is currently building and a build-order is given
				// to the former
				if (fcai == nullptr && bcai == nullptr)
					return false;

				// {Builder,Factory}CAI::GiveCommandReal (should) handle the
				// case where buildOptions.find(cmdID) == buildOptions.end()
			}
		} break;
	}


	const UnitDef* ud = owner->unitDef;
	// AI's may issue attack-ground orders that are not on the ground
	const CSkirmishAIHandler::ids_t& saids = skirmishAIHandler.GetSkirmishAIsInTeam(owner->team);

	const bool npOrder = c.params.empty(); // no-param
	const bool aiOrder = !saids.empty();

	switch (cmdID) {
		case CMD_MANUALFIRE: {
			if (!ud->canManualFire)
				return false;
		} // fall through

		case CMD_ATTACK: {
			if (!IsAttackCapable())
				return false;

			if (c.params.size() == 1) {
				const CUnit* attackee = GetCommandUnit(c, 0);

				if (attackee == nullptr)
					return false;
				if (!attackee->pos.IsInBounds())
					return false;
			} else {
				AdjustGroundAttackCommand(c, fromSynced, aiOrder);
			}
		} break;

		case CMD_MOVE: {
			if (!ud->canmove)
				return false;
		} break;
		case CMD_FIGHT: {
			if (!ud->canFight)
				return false;
		} break;
		case CMD_GUARD: {
			const CUnit* guardee = GetCommandUnit(c, 0);

			if (!ud->canGuard)
				return false;
			if (owner && !owner->pos.IsInBounds())
				return false;
			if (guardee && !guardee->pos.IsInBounds())
				return false;
		} break;

		case CMD_PATROL: {
			if (!ud->canPatrol)
				return false;
		} break;

		case CMD_CAPTURE: {
			const CUnit* capturee = GetCommandUnit(c, 0);

			if (!ud->canCapture)
				return false;
			if (capturee != nullptr && !capturee->pos.IsInBounds())
				return false;
		} break;

		case CMD_RECLAIM: {
			const CUnit* reclaimeeUnit = GetCommandUnit(c, 0);
			const CFeature* reclaimeeFeature = nullptr;

			if (c.IsAreaCommand())
				return true;
			if (!ud->canReclaim)
				return false;

			if (reclaimeeUnit != nullptr) {
				if (!reclaimeeUnit->unitDef->reclaimable)
					return false;
				if (!reclaimeeUnit->AllowedReclaim(owner))
					return false;
				if (!reclaimeeUnit->pos.IsInBounds())
					return false;
			} else {
				const unsigned int reclaimeeFeatureID = (!npOrder)? c.GetParam(0): 0;

				if (reclaimeeFeatureID >= unitHandler->MaxUnits()) {
					reclaimeeFeature = featureHandler->GetFeature(reclaimeeFeatureID - unitHandler->MaxUnits());

					if (reclaimeeFeature != nullptr && !reclaimeeFeature->def->reclaimable)
						return false;
				}
			}
		} break;

		case CMD_RESTORE: {
			if (!ud->canRestore || mapDamage->disabled)
				return false;
		} break;

		case CMD_RESURRECT: {
			if (!ud->canResurrect)
				return false;
		} break;

		case CMD_REPAIR: {
			const CUnit* repairee = GetCommandUnit(c, 0);

			if (!ud->canRepair && !ud->canAssist)
				return false;

			if (repairee != nullptr && !repairee->pos.IsInBounds())
				return false;
			if (repairee != nullptr && ((repairee->beingBuilt && !ud->canAssist) || (!repairee->beingBuilt && !ud->canRepair)))
				return false;
		} break;
	}


	if (cmdID == CMD_FIRE_STATE && (npOrder || !CanChangeFireState()))
		return false;
	if (cmdID == CMD_MOVE_STATE && (npOrder || (!ud->canmove && !ud->builder)))
		return false;

	if (cmdID == CMD_REPEAT && (npOrder || !ud->canRepeat || ((int)c.params[0] % 2) != (int)c.params[0]/* only 0 or 1 allowed */))
		return false;

	if (cmdID == CMD_TRAJECTORY && (npOrder || ud->highTrajectoryType < 2))
		return false;

	if (cmdID == CMD_ONOFF && (npOrder || !ud->onoffable || owner->beingBuilt || ((int)c.params[0] % 2) != (int)c.params[0]/* only 0 or 1 allowed */))
		return false;

	if (cmdID == CMD_CLOAK && (npOrder || !ud->canCloak || ((int)c.params[0] % 2) != (int)c.params[0]/* only 0 or 1 allowed */))
		return false;

	if (cmdID == CMD_STOCKPILE && !stockpileWeapon)
		return false;

	return true;
}


void CCommandAI::GiveCommand(const Command& c, bool fromSynced)
{
	if (!eventHandler.AllowCommand(owner, c, fromSynced))
		return;

	eventHandler.UnitCommand(owner, c);
	this->GiveCommandReal(c, fromSynced); // send to the sub-classes
}


void CCommandAI::GiveCommandReal(const Command& c, bool fromSynced)
{
	if (!AllowedCommand(c, fromSynced))
		return;

	GiveAllowedCommand(c, fromSynced);
}


inline void CCommandAI::SetCommandDescParam0(const Command& c)
{
	for (unsigned int n = 0; n < possibleCommands.size(); n++) {
		if (possibleCommands[n]->id != c.GetID())
			continue;

		UpdateCommandDescription(n, c);
		break;
	}
}


bool CCommandAI::ExecuteStateCommand(const Command& c)
{
	switch (c.GetID()) {
		case CMD_FIRE_STATE: {
			owner->fireState = (int)c.params[0];
			SetCommandDescParam0(c);
			selectedUnitsHandler.PossibleCommandChange(owner);
			return true;
		}
		case CMD_MOVE_STATE: {
			owner->moveState = (int)c.params[0];
			SetCommandDescParam0(c);
			selectedUnitsHandler.PossibleCommandChange(owner);
			return true;
		}
		case CMD_REPEAT: {
			if (c.params[0] == 1) {
				repeatOrders = true;
			} else if(c.params[0] == 0) {
				repeatOrders = false;
			} else {
				// cause some code parts need it to be either 0 or 1,
				// we can not accept any other values as valid
				return false;
			}
			SetCommandDescParam0(c);
			selectedUnitsHandler.PossibleCommandChange(owner);
			return true;
		}
		case CMD_TRAJECTORY: {
			owner->useHighTrajectory = !!c.params[0];
			SetCommandDescParam0(c);
			selectedUnitsHandler.PossibleCommandChange(owner);
			return true;
		}
		case CMD_ONOFF: {
			if (c.params[0] == 1) {
				owner->Activate();
			} else if (c.params[0] == 0) {
				owner->Deactivate();
			} else {
				// cause some code parts need it to be either 0 or 1,
				// we can not accept any other values as valid
				return false;
			}
			SetCommandDescParam0(c);
			selectedUnitsHandler.PossibleCommandChange(owner);
			return true;
		}
		case CMD_CLOAK: {
			if (c.params[0] == 1) {
				owner->wantCloak = true;
			} else if(c.params[0] == 0) {
				owner->wantCloak = false;
				owner->curCloakTimeout = gs->frameNum + owner->cloakTimeout;
			} else {
				// cause some code parts need it to be either 0 or 1,
				// we can not accept any other values as valid
				return false;
			}
			SetCommandDescParam0(c);
			selectedUnitsHandler.PossibleCommandChange(owner);
			return true;
		}
		case CMD_STOCKPILE: {
			int change = 1;
			if (c.options & RIGHT_MOUSE_KEY) { change *= -1; }
			if (c.options & SHIFT_KEY)       { change *=  5; }
			if (c.options & CONTROL_KEY)     { change *= 20; }

			stockpileWeapon->numStockpileQued += change;
			stockpileWeapon->numStockpileQued = std::max(stockpileWeapon->numStockpileQued, 0);

			UpdateStockpileIcon();
			return true;
		}
	}

	// if this is a custom lua command, call CommandFallback
	// (ignoring the result) since it can't stay in the queue
	if (nonQueingCommands.find(c.GetID()) != nonQueingCommands.end()) {
		eventHandler.CommandFallback(owner, c);
		return true;
	}

	return false;
}


void CCommandAI::ClearTargetLock(const Command &c) {
	if (((c.GetID() == CMD_ATTACK) || (c.GetID() == CMD_MANUALFIRE)) && (c.options & META_KEY) == 0) {
		// no meta-bit attack lock, clear the order
		owner->DropCurrentAttackTarget();
	}
}


void CCommandAI::GiveAllowedCommand(const Command& c, bool fromSynced)
{
	if (ExecuteStateCommand(c))
		return;

	switch (c.GetID()) {
		case CMD_SELFD: {
			if (owner->unitDef->canSelfD) {
				if (!(c.options & SHIFT_KEY) || commandQue.empty()) {
					if (owner->selfDCountdown != 0) {
						owner->selfDCountdown = 0;
					} else {
						owner->selfDCountdown = owner->unitDef->selfDCountdown*2+1;
					}
				}
				else if (commandQue.back().GetID() == CMD_SELFD) {
					commandQue.pop_back();
				} else {
					commandQue.push_back(c);
				}
			}
			return;
		}
		case CMD_SET_WANTED_MAX_SPEED: {
			if (CanSetMaxSpeed() &&
			    (commandQue.empty() ||
			     (commandQue.back().GetID() != CMD_SET_WANTED_MAX_SPEED))) {
				// bail early, do not check for overlaps or queue cancelling
				commandQue.push_back(c);
				if (commandQue.size()==1 && !owner->beingBuilt) {
					SlowUpdate();
				}
			}
			return;
		}
		case CMD_WAIT: {
			GiveWaitCommand(c);
			return;
		}
		case CMD_INSERT: {
			ExecuteInsert(c, fromSynced);
			return;
		}
		case CMD_REMOVE: {
			ExecuteRemove(c);
			return;
		}
	}

	// flush the queue for immediate commands
	// NOTE: CMD_STOP can be a queued order (!)
	if (!(c.options & SHIFT_KEY)) {
		waitCommandsAI.ClearUnitQueue(owner, commandQue);
		ClearTargetLock((commandQue.empty())? Command(CMD_STOP): commandQue.front());
		ClearCommandDependencies();
		SetOrderTarget(nullptr);

		// if c is an attack command, the actual order-target
		// gets set via ExecuteAttack (called from SlowUpdate
		// at the end of this function)
		commandQue.clear();
		assert(commandQue.empty());

		inCommand = false;
	}

	AddCommandDependency(c);

	if (c.GetID() == CMD_PATROL) {
		CCommandQueue::iterator ci = commandQue.begin();
		for (; ci != commandQue.end() && ci->GetID() != CMD_PATROL; ++ci) {
			// just increment
		}
		if (ci == commandQue.end()) {
			if (commandQue.empty()) {
				Command c2(CMD_PATROL, c.options, owner->pos);
				commandQue.push_back(c2);
			} else {
				do {
					--ci;
					if (ci->params.size() >= 3) {
						Command c2(CMD_PATROL, c.options);
						c2.params = ci->params;
						commandQue.push_back(c2);
						break;
					} else if (ci == commandQue.begin()) {
						Command c2(CMD_PATROL, c.options, owner->pos);
						commandQue.push_back(c2);
						break;
					}
				}
				while (ci != commandQue.begin());
			}
		}
	}

	// cancel duplicated commands
	bool first;
	if (CancelCommands(c, commandQue, first) > 0) {
		if (first) {
			Command stopCommand(CMD_STOP);
			commandQue.push_front(stopCommand);
			SlowUpdate();
		}
		return;
	}

	// do not allow overlapping commands
	if (!GetOverlapQueued(c).empty())
		return;

	if (c.GetID() == CMD_ATTACK) {
		// avoid weaponless units moving to 0 distance when given attack order
		if (owner->weapons.empty() && (!owner->unitDef->canKamikaze)) {
			Command c2(CMD_STOP);
			commandQue.push_back(c2);
			return;
		}
	}

	commandQue.push_back(c);

	if (commandQue.size() == 1 && !owner->beingBuilt && !owner->IsStunned()) {
		SlowUpdate();
	}
}


void CCommandAI::GiveWaitCommand(const Command& c)
{
	if (commandQue.empty()) {
		commandQue.push_back(c);
		return;
	}
	else if (c.options & SHIFT_KEY) {
		if (commandQue.back().GetID() == CMD_WAIT) {
			waitCommandsAI.RemoveWaitCommand(owner, commandQue.back());
			commandQue.pop_back();
		} else {
			commandQue.push_back(c);
			return;
		}
	}
	else if (commandQue.front().GetID() == CMD_WAIT) {
		waitCommandsAI.RemoveWaitCommand(owner, commandQue.front());
		commandQue.pop_front();
		return;
	}
	else {
		// shutdown the current order
		owner->DropCurrentAttackTarget();
		StopMove();
		inCommand = false;
		targetDied = false;

		commandQue.push_front(c);
		return;
	}

	if (commandQue.empty()) {
		if (!owner->group) {
			eoh->UnitIdle(*owner);
		}
		eventHandler.UnitIdle(owner);
	} else {
		SlowUpdate();
	}
}


void CCommandAI::ExecuteInsert(const Command& c, bool fromSynced)
{
	if (c.params.size() < 3)
		return;

	// make the command
	Command newCmd((int)c.params[1], (unsigned char)c.params[2]);
	for (int p = 3; p < (int)c.params.size(); p++) {
		newCmd.PushParam(c.params[p]);
	}

	// validate the command
	if (!AllowedCommand(newCmd, fromSynced))
		return;

	CCommandQueue* queue = &commandQue;

	bool facBuildQueue = false;
	CFactoryCAI* facCAI = dynamic_cast<CFactoryCAI*>(this);
	if (facCAI != nullptr) {
		if (c.options & CONTROL_KEY) {
			// check the build order
			const auto& bOpts = facCAI->buildOptions;
			if ((newCmd.GetID() != CMD_STOP) && (newCmd.GetID() != CMD_WAIT) &&
			    ((newCmd.GetID() >= 0) || (bOpts.find(newCmd.GetID()) == bOpts.end()))) {
				return;
			}
			facBuildQueue = true;
		} else {
			// use the new commands
			queue = &facCAI->newUnitCommands;
		}
	}

	CCommandQueue::iterator insertIt = queue->begin();

	if (c.options & ALT_KEY) {
		// treat param0 as a position
		int pos = (int)c.params[0];
		const int qsize = queue->size();

		// convert negative indices, leave positive values untouched
		pos = std::max(0, pos + (qsize + 1) * (pos < 0));
		pos = std::min(pos, qsize);

		std::advance(insertIt, pos);
	} else {
		// treat param0 as a command tag
		const unsigned int tag = (unsigned int)c.params[0];

		bool found = false;

		for (auto ci = queue->begin(); ci != queue->end(); ++ci) {
			const Command& qc = *ci;

			if ((found = (qc.tag == tag))) {
				insertIt = ci;
				break;
			}
		}

		if (!found)
			return;

		if ((c.options & RIGHT_MOUSE_KEY) && (insertIt != queue->end())) {
			++insertIt; // insert after the tagged command
		}
	}

	if (facBuildQueue) {
		facCAI->InsertBuildCommand(insertIt, newCmd);

		if (!owner->IsStunned())
			SlowUpdate();

		return;
	}

	// shutdown the current order if the insertion is at the beginning
	if (!queue->empty() && (insertIt == queue->begin())) {
		inCommand = false;
		targetDied = false;

		SetOrderTarget(nullptr);
		const Command& cmd = queue->front();
		eoh->CommandFinished(*owner, cmd);
		eventHandler.UnitCmdDone(owner, cmd);
		ClearTargetLock(cmd);
	}

	queue->insert(insertIt, newCmd);

	if (owner->IsStunned())
		return;

	SlowUpdate();
}


void CCommandAI::ExecuteRemove(const Command& c)
{
	CCommandQueue* queue = &commandQue;
	CFactoryCAI* facCAI = dynamic_cast<CFactoryCAI*>(this);

	// if false, remove commands by tag
	const bool removeByID = (c.options & ALT_KEY);
	// disable repeating during the removals
	const bool prevRepeat = repeatOrders;

	// erase commands by a list of command types
	bool active = false;
	bool facBuildQueue = false;

	if (facCAI) {
		if (c.options & CONTROL_KEY) {
			// keep using the build-order queue
			facBuildQueue = true;
		} else {
			// use the command-queue for new units
			queue = &facCAI->newUnitCommands;
		}
	}

	if ((c.params.size() <= 0) || (queue->size() <= 0)) {
		return;
	}

	repeatOrders = false;

	for (unsigned int p = 0; p < c.params.size(); p++) {
		const int removeValue = c.params[p]; // tag or id

		if (facBuildQueue && !removeByID && (removeValue == 0)) {
			// don't remove commands with tag 0 from build queues, they
			// are used the same way that CMD_STOP is (to void orders)
			continue;
		}

		CCommandQueue::iterator ci;

		do {
			for (ci = queue->begin(); ci != queue->end(); ++ci) {
				const Command& qc = *ci;

				if (removeByID) {
					if (qc.GetID() != removeValue)  { continue; }
				} else {
					if (qc.tag != removeValue) { continue; }
				}

				if (qc.GetID() == CMD_WAIT) {
					waitCommandsAI.RemoveWaitCommand(owner, qc);
				}

				if (facBuildQueue) {
					// if ci == queue->begin() and !queue->empty(), this pop_front()'s
					// via CFAI::ExecuteStop; otherwise only modifies *ci (not <queue>)
					if (facCAI->RemoveBuildCommand(ci)) {
						ci = queue->begin(); break;
					}
				}

				if (!facCAI && (ci == queue->begin())) {
					if (!active) {
						active = true;
						FinishCommand();
						ci = queue->begin();
						break;
					}
					active = true;
				}

				queue->erase(ci);
				ci = queue->begin();

				// the removal may have corrupted the iterator
				break;
			}
		}
		while (ci != queue->end());
	}

	repeatOrders = prevRepeat;
}


bool CCommandAI::WillCancelQueued(const Command& c)
{
	return (GetCancelQueued(c, commandQue) != commandQue.end());
}


CCommandQueue::iterator CCommandAI::GetCancelQueued(const Command& c, CCommandQueue& q)
{
	CCommandQueue::iterator ci = q.end();

	while (ci != q.begin()) {
		--ci; //iterate from the end and dont check the current order
		const Command& c2 = *ci;
		const int cmdID = c.GetID();
		const int cmd2ID = c2.GetID();

		const bool attackAndFight = (cmdID == CMD_ATTACK && cmd2ID == CMD_FIGHT && c2.params.size() == 1);

		if (c2.params.size() != c.params.size())
			continue;

		if ((cmdID == cmd2ID) || (cmdID < 0 && cmd2ID < 0) || attackAndFight) {
			if (c.params.size() == 1) {
				// assume the param is a unit-ID or feature-ID
				if ((c2.params[0] == c.params[0]) &&
				    (cmd2ID != CMD_SET_WANTED_MAX_SPEED)) {
					return ci;
				}
			}
			else if (c.params.size() >= 3) {
				if (cmdID < 0) {
					BuildInfo bc1(c);
					BuildInfo bc2(c2);

					if (bc1.def == NULL) continue;
					if (bc2.def == NULL) continue;

					if (math::fabs(bc1.pos.x - bc2.pos.x) * 2 <= std::max(bc1.GetXSize(), bc2.GetXSize()) * SQUARE_SIZE &&
					    math::fabs(bc1.pos.z - bc2.pos.z) * 2 <= std::max(bc1.GetZSize(), bc2.GetZSize()) * SQUARE_SIZE) {
						return ci;
					}
				} else {
					// assume c and c2 are positional commands
					const float3& c1p = c.GetPos(0);
					const float3& c2p = c2.GetPos(0);

					if ((c1p - c2p).SqLength2D() >= (COMMAND_CANCEL_DIST * COMMAND_CANCEL_DIST))
						continue;
					if ((c.options & SHIFT_KEY) != 0 && (c.options & INTERNAL_ORDER) != 0)
						continue;

					return ci;
				}
			}
		}
	}
	return q.end();
}


int CCommandAI::CancelCommands(const Command& c, CCommandQueue& q, bool& first)
{
	first = false;
	int cancelCount = 0;

	while (true) {
		CCommandQueue::iterator ci = GetCancelQueued(c, q);

		if (ci == q.end())
			return cancelCount;

		first = first || (ci == q.begin());
		cancelCount++;

		CCommandQueue::iterator firstErase = ci;
		CCommandQueue::iterator lastErase = ci;

		++ci;
		if ((ci != q.end()) && (ci->GetID() == CMD_SET_WANTED_MAX_SPEED)) {
			lastErase = ci;
			cancelCount++;
			++ci;
		}

		if ((ci != q.end()) && (ci->GetID() == CMD_WAIT)) {
			waitCommandsAI.RemoveWaitCommand(owner, *ci);
			lastErase = ci;
			cancelCount++;
			++ci;
		}

		++lastErase; // STL: erase the range [first, last)
		q.erase(firstErase, lastErase);

		if (c.GetID() >= 0)
			return cancelCount; // only delete one non-build order
	}

	return cancelCount;
}


std::vector<Command> CCommandAI::GetOverlapQueued(const Command& c) const
{
	return GetOverlapQueued(c, commandQue);
}


std::vector<Command> CCommandAI::GetOverlapQueued(const Command& c, const CCommandQueue& q) const
{
	CCommandQueue::const_iterator ci = q.end();
	std::vector<Command> v;
	BuildInfo cbi(c);

	if (ci != q.begin()) {
		do {
			--ci; //iterate from the end and dont check the current order
			const Command& t = *ci;

			if (t.params.size() != c.params.size())
				continue;

			if (t.GetID() == c.GetID() || (c.GetID() < 0 && t.GetID() < 0)) {
				if (c.params.size() == 1) {
					// assume the param is a unit or feature id
					if (t.params[0] == c.params[0]) {
						v.push_back(t);
					}
				}
				else if (c.params.size() >= 3) {
					// assume c and t are positional commands
					// NOTE: uses a BuildInfo structure, but <t> can be ANY command
					BuildInfo tbi;
					if (tbi.Parse(t)) {
						const float dist2X = 2.0f * math::fabs(cbi.pos.x - tbi.pos.x);
						const float dist2Z = 2.0f * math::fabs(cbi.pos.z - tbi.pos.z);
						const float addSizeX = SQUARE_SIZE * (cbi.GetXSize() + tbi.GetXSize());
						const float addSizeZ = SQUARE_SIZE * (cbi.GetZSize() + tbi.GetZSize());
						const float maxSizeX = SQUARE_SIZE * std::max(cbi.GetXSize(), tbi.GetXSize());
						const float maxSizeZ = SQUARE_SIZE * std::max(cbi.GetZSize(), tbi.GetZSize());

						if (cbi.def == NULL) continue;
						if (tbi.def == NULL) continue;

						if (((dist2X > maxSizeX) || (dist2Z > maxSizeZ)) &&
						    ((dist2X < addSizeX) && (dist2Z < addSizeZ))) {
							v.push_back(t);
						}
					} else {
						if ((cbi.pos - tbi.pos).SqLength2D() >= (COMMAND_CANCEL_DIST * COMMAND_CANCEL_DIST))
							continue;
						if ((c.options & SHIFT_KEY) != 0 && (c.options & INTERNAL_ORDER) != 0)
							continue;

						v.push_back(t);
					}
				}
			}
		} while (ci != q.begin());
	}
	return v;
}


int CCommandAI::UpdateTargetLostTimer(int targetUnitID)
{
	const CUnit* targetUnit = unitHandler->GetUnit(targetUnitID);
	const UnitDef* targetUnitDef = (targetUnit != NULL)? targetUnit->unitDef: NULL;

	if (targetUnit == NULL)
		return (targetLostTimer = 0);

	if (targetUnitDef->IsImmobileUnit())
		return (targetLostTimer = TARGET_LOST_TIMER);

	// keep tracking so long as target is on radar (or indefinitely if immobile)
	if ((targetUnit->losStatus[owner->allyteam] & LOS_INRADAR))
		return (targetLostTimer = TARGET_LOST_TIMER);

	return (std::max(--targetLostTimer, 0));
}


void CCommandAI::ExecuteAttack(Command& c)
{
	assert(owner->unitDef->canAttack);

	if (inCommand) {
		if (targetDied || (c.params.size() == 1 && UpdateTargetLostTimer(int(c.params[0])) == 0)) {
			FinishCommand();
			return;
		}
		if (!(c.options & ALT_KEY) && SkipParalyzeTarget(orderTarget)) {
			FinishCommand();
			return;
		}
	} else {
		if (c.params.size() == 1) {
			CUnit* targetUnit = unitHandler->GetUnit(c.params[0]);

			if (targetUnit == NULL) { FinishCommand(); return; }
			if (targetUnit == owner) { FinishCommand(); return; }
			if (targetUnit->GetTransporter() != NULL && !modInfo.targetableTransportedUnits) {
				FinishCommand(); return;
			}

			SetOrderTarget(targetUnit);
			owner->AttackUnit(targetUnit, (c.options & INTERNAL_ORDER) == 0, c.GetID() == CMD_MANUALFIRE);
			inCommand = true;
		} else {
			owner->AttackGround(c.GetPos(0), (c.options & INTERNAL_ORDER) == 0, c.GetID() == CMD_MANUALFIRE);
			inCommand = true;
		}
	}
}


void CCommandAI::ExecuteStop(Command &c)
{
	owner->DropCurrentAttackTarget();

	for (CWeapon* w: owner->weapons) {
		w->DropCurrentTarget();
	}

	FinishCommand();
}


void CCommandAI::SlowUpdate()
{
	if (gs->paused) // Commands issued may invoke SlowUpdate when paused
		return;
	if (commandQue.empty()) {
		return;
	}

	Command& c = commandQue.front();

	switch (c.GetID()) {
		case CMD_WAIT: {
			return;
		}
		case CMD_SELFD: {
			if ((owner->selfDCountdown != 0) || !owner->unitDef->canSelfD) {
				owner->selfDCountdown = 0;
			} else {
				owner->selfDCountdown = (owner->unitDef->selfDCountdown * 2) + 1;
			}
			FinishCommand();
			return;
		}
		case CMD_STOP: {
			ExecuteStop(c);
			return;
		}
		case CMD_ATTACK: {
			ExecuteAttack(c);
			return;
		}
		case CMD_MANUALFIRE: {
			ExecuteAttack(c);
			return;
		}
	}

	if (ExecuteStateCommand(c)) {
		FinishCommand();
		return;
	}

	// luaRules wants the command to stay at the front
	if (!eventHandler.CommandFallback(owner, c))
		return;

	FinishCommand();
}


int CCommandAI::GetDefaultCmd(const CUnit* pointed, const CFeature* feature)
{
	if (pointed) {
		if (!teamHandler->Ally(gu->myAllyTeam, pointed->allyteam)) {
			if (IsAttackCapable()) {
				return CMD_ATTACK;
			}
		}
	}
	return CMD_STOP;
}


void CCommandAI::AddDeathDependence(CObject* o, DependenceType dep) {
	if (dep == DEPENDENCE_COMMANDQUE) {
		if (commandDeathDependences.insert(o).second) // prevent multiple dependencies for the same object
			CObject::AddDeathDependence(o, dep);
		return;
	}
	CObject::AddDeathDependence(o, dep);
}


void CCommandAI::DeleteDeathDependence(CObject* o, DependenceType dep) {
	if (dep == DEPENDENCE_COMMANDQUE) {
		if (commandDeathDependences.erase(o))
			CObject::DeleteDeathDependence(o, dep);
		return;
	}
	CObject::DeleteDeathDependence(o,dep);
}


void CCommandAI::DependentDied(CObject* o)
{
	if (o == orderTarget) {
		targetDied = true;
		orderTarget = NULL;
	}

	if (commandDeathDependences.erase(o) && o != owner) {
		CFactoryCAI* facCAI = dynamic_cast<CFactoryCAI*>(this);
		CCommandQueue& dq = facCAI ? facCAI->newUnitCommands : commandQue;
		int lastTag;
		int curTag = -1;
		do {
			lastTag = curTag;
			for (CCommandQueue::iterator qit = dq.begin(); qit != dq.end(); ++qit) {
				Command &c = *qit;
				int cpos;
				if (c.IsObjectCommand(cpos) && (c.params[cpos] == CSolidObject::GetDeletingRefID())) {
					curTag = c.tag;
					Command removeCmd(CMD_REMOVE, 0, curTag);
					ExecuteRemove(removeCmd);
					break;
				}
			}
		} while(curTag != lastTag);
	}
}



void CCommandAI::FinishCommand()
{
	assert(!commandQue.empty());

	const Command cmd = commandQue.front(); //cppcheck false positive, copy is needed here

	const bool dontRepeat = (cmd.options & INTERNAL_ORDER);
	const bool pushCommand = (cmd.GetID() != CMD_STOP && cmd.GetID() != CMD_PATROL && cmd.GetID() != CMD_SET_WANTED_MAX_SPEED);

	if (repeatOrders && !dontRepeat && pushCommand)
		commandQue.push_back(cmd);

	commandQue.pop_front();

	inCommand = false;
	targetDied = false;

	SetOrderTarget(nullptr);
	eoh->CommandFinished(*owner, cmd);
	eventHandler.UnitCmdDone(owner, cmd);
	ClearTargetLock(cmd);

	if (commandQue.empty()) {
		if (owner->group == nullptr)
			eoh->UnitIdle(*owner);

		eventHandler.UnitIdle(owner);
	}

	// avoid infinite loops
	if (lastFinishCommand == gs->frameNum)
		return;

	lastFinishCommand = gs->frameNum;

	if (owner->IsStunned())
		return;

	SlowUpdate();
}

void CCommandAI::AddStockpileWeapon(CWeapon* weapon)
{
	stockpileWeapon = weapon;

	SCommandDescription c;

	c.id   = CMD_STOCKPILE;
	c.type = CMDTYPE_ICON;

	c.action   = "stockpile";
	c.name     = "0/0";
	c.tooltip  = c.action + ": Queue up ammunition for later use";
	c.iconname = "bitmaps/armsilo1.bmp";

	possibleCommands.push_back(commandDescriptionCache->GetPtr(c));
}

void CCommandAI::StockpileChanged(CWeapon* weapon)
{
	UpdateStockpileIcon();
}

void CCommandAI::UpdateStockpileIcon()
{
	for (unsigned int n = 0; n < possibleCommands.size(); n++) {
		if (possibleCommands[n]->id != CMD_STOCKPILE)
			continue;
		SCommandDescription c = *possibleCommands[n];
		c.name =
			IntToString(stockpileWeapon->numStockpiled                                    ) + "/" +
			IntToString(stockpileWeapon->numStockpiled + stockpileWeapon->numStockpileQued);
		possibleCommands[n] = commandDescriptionCache->GetPtr(c);

		selectedUnitsHandler.PossibleCommandChange(owner);
		break;
	}
}

void CCommandAI::WeaponFired(CWeapon* weapon, const bool searchForNewTarget)
{
	if (!inCommand)
		return;

	const Command& c = commandQue.front();

	const bool haveGroundAttackCmd = (c.GetID() == CMD_ATTACK && c.GetParamsCount() >= 3);
	const bool haveAreaAttackCmd = (c.GetID() == CMD_AREA_ATTACK);

	bool orderFinished = false;

	if (searchForNewTarget) {
		// manual fire or attack commands with meta will only fire a single salvo
		// noAutoTarget weapons finish an attack commands after a
		// salvo if they have more orders queued
		if (weapon->weaponDef->manualfire && !(c.options & META_KEY))
			orderFinished = true;

		if (weapon->noAutoTarget && !(c.options & META_KEY) && haveGroundAttackCmd && HasMoreMoveCommands())
			orderFinished = true;

		// if we have an area-attack command and this was the
		// last salvo of our main weapon, assume we completed an attack
		// (run) on one position and move to the next
		//
		// if we have >= 2 consecutive CMD_ATTACK's, then
		//   SelectNAATP --> FinishCommand (inCommand=false) -->
		//   SlowUpdate --> ExecuteAttack (inCommand=true) -->
		//   queue has advanced
		//
		// @SelectNewAreaAttackTargetOrPos
		// return argument says if a new area attack target was chosen, else finish current command
		if (haveAreaAttackCmd) {
			orderFinished = !SelectNewAreaAttackTargetOrPos(c);
		}
	}

	// if this fails, we need to take a copy at top instead of a reference
	assert(&c == &commandQue.front());

	eoh->WeaponFired(*owner, *(weapon->weaponDef));

	if (!orderFinished)
		return;

	FinishCommand();
}

void CCommandAI::PushOrUpdateReturnFight(const float3& cmdPos1, const float3& cmdPos2)
{
	assert(!commandQue.empty());
	Command& c = commandQue.front();
	assert(c.GetID() == CMD_FIGHT && c.params.size() >= 3);

	const float3 pos = ClosestPointOnLine(cmdPos1, cmdPos2, owner->pos);
	if (c.params.size() >= 6) {
		c.SetPos(0, pos);
	} else {
		// make the new fight command inherit <c>'s options
		Command c2(CMD_FIGHT, c.options, pos);
		c2.PushPos(c.GetPos(0));
		commandQue.push_front(c2);
	}
}


bool CCommandAI::HasCommand(int cmdID) const {
	if (commandQue.empty())
		return false;
	if (cmdID < 0)
		return ((commandQue.front()).IsBuildCommand());

	return ((commandQue.front()).GetID() == cmdID);
}

bool CCommandAI::HasMoreMoveCommands(bool skipFirstCmd) const
{
	if (commandQue.empty())
		return false;

	auto i = commandQue.begin();

	if (skipFirstCmd)
		++i;

	for (; i != commandQue.end(); ++i) {
		if (i->IsMoveCommand())
			return true;
	}

	return false;
}


bool CCommandAI::SkipParalyzeTarget(const CUnit* target)
{
	// check to see if we are about to paralyze a unit that is already paralyzed
	if ((target == nullptr) || (owner->weapons.empty()))
		return false;

	const CWeapon* w = owner->weapons.front();

	if (!w->weaponDef->paralyzer)
		return false;

	// visible and stunned?
	if ((target->losStatus[owner->allyteam] & LOS_INLOS) && target->IsStunned() && HasMoreMoveCommands())
		return true;

	return false;
}

bool CCommandAI::CanChangeFireState() { return (owner->unitDef->CanChangeFireState()); }


void CCommandAI::StopAttackingAllyTeam(int ally)
{
	std::vector<int> todel;

	// erasing in the middle invalidates all iterators
	for (auto it = commandQue.begin(); it != commandQue.end(); ++it) {
		const Command& c = *it;

		if (c.params.size() != 1)
			continue;
		if (c.GetID() != CMD_FIGHT && c.GetID() != CMD_ATTACK)
			continue;

		const CUnit* target = unitHandler->GetUnit(c.params[0]);

		if (target == nullptr)
			continue;
		if (target->allyteam != ally)
			continue;

		todel.push_back(it - commandQue.begin());
	}

	for (auto it = todel.rbegin(); it != todel.rend(); ++it) {
		commandQue.erase(commandQue.begin() + *it);
	}
}



void CCommandAI::SetScriptMaxSpeed(float speed, bool persistent) {
	if (!persistent) {
		// find the first CMD_SET_WANTED_MAX_SPEED and modify it
		// NOTE:
		//     this has no effect if the unit does not already have
		//     such an order, and only lasts until a new move-order
		//     is given (hence non-persistent)
		CCommandQueue::iterator it;

		for (it = commandQue.begin(); it != commandQue.end(); ++it) {
			Command& c = *it;

			if (c.GetID() != CMD_SET_WANTED_MAX_SPEED)
				continue;

			owner->moveType->SetWantedMaxSpeed(c.params[0] = speed);
			break;
		}
	} else {
		// permanently change the unit's speed
		owner->moveType->SetMaxSpeed(speed);
	}
}

void CCommandAI::SlowUpdateMaxSpeed() {
	if (commandQue.size() < 2)
		return;

	// grab the second command
	const CCommandQueue::const_iterator it = ++(commandQue.begin());
	const Command& c = *it;

	// treat any following CMD_SET_WANTED_MAX_SPEED commands as options
	// to the current command (and ignore them when it's their turn)
	if (c.GetID() != CMD_SET_WANTED_MAX_SPEED)
		return;

	assert(!c.params.empty());
	owner->moveType->SetWantedMaxSpeed(c.params[0]);
}

