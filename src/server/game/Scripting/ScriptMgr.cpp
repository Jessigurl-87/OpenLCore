/*
 * Copyright (C) 2008-2018 TrinityCore <https://www.trinitycore.org/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "ScriptMgr.h"
#include "Area.h"
#include "AreaTrigger.h"
#include "AreaTriggerAI.h"
#include "Chat.h"
#include "Conversation.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "CreatureAIImpl.h"
#include "Errors.h"
#include "GameObject.h"
#include "Garrison.h"
#include "GarrisonAI.h"
#include "GossipDef.h"
#include "Item.h"
#include "LFGScripts.h"
#include "Log.h"
#include "Map.h"
#include "MapManager.h"
#include "ObjectMgr.h"
#include "OutdoorPvPMgr.h"
#include "Player.h"
#include "ScriptReloadMgr.h"
#include "ScriptSystem.h"
#include "SmartAI.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "Timer.h"
#include "Transport.h"
#include "Vehicle.h"
#include "Weather.h"
#include "WorldPacket.h"
#include "ZoneScript.h"
#include <unordered_map>
#include "Unit.h"

// Trait which indicates whether this script type
// must be assigned in the database.
template<typename>
struct is_script_database_bound
    : std::false_type { };

template<>
struct is_script_database_bound<SpellScriptLoader>
    : std::true_type { };

template<>
struct is_script_database_bound<InstanceMapScript>
    : std::true_type { };

template<>
struct is_script_database_bound<ItemScript>
    : std::true_type { };

template<>
struct is_script_database_bound<CreatureScript>
    : std::true_type { };

template<>
struct is_script_database_bound<GameObjectScript>
    : std::true_type { };

template<>
struct is_script_database_bound<VehicleScript>
    : std::true_type { };

template<>
struct is_script_database_bound<AreaTriggerScript>
    : std::true_type { };

template<>
struct is_script_database_bound<BattlegroundScript>
    : std::true_type { };

template<>
struct is_script_database_bound<OutdoorPvPScript>
    : std::true_type { };

template<>
struct is_script_database_bound<WeatherScript>
    : std::true_type { };

template<>
struct is_script_database_bound<ConditionScript>
    : std::true_type { };

template<>
struct is_script_database_bound<TransportScript>
    : std::true_type { };

template<>
struct is_script_database_bound<AchievementCriteriaScript>
    : std::true_type { };

template<>
struct is_script_database_bound<AreaTriggerEntityScript>
    : std::true_type { };

template<>
struct is_script_database_bound<GarrisonScript>
    : std::true_type { };

template<>
struct is_script_database_bound<ConversationScript>
    : std::true_type { };

template<>
struct is_script_database_bound<SceneScript>
    : std::true_type { };

template<>
struct is_script_database_bound<QuestScript>
    : std::true_type { };

template<>
struct is_script_database_bound<ZoneScript>
    : std::true_type { };

template<>
struct is_script_database_bound<AllCreatureScript>
    : std::true_type { };

template<>
struct is_script_database_bound<ChannelScript>
    : std::true_type { };

template<>
struct is_script_database_bound<MovementHandlerScript>
    : std::true_type { };

template<>
struct is_script_database_bound<SocialScript>
    : std::true_type { };

enum Spells
{
    SPELL_HOTSWAP_VISUAL_SPELL_EFFECT = 40162 // 59084
};

class ScriptRegistryInterface
{
public:
    ScriptRegistryInterface() { }
    virtual ~ScriptRegistryInterface() { }

    ScriptRegistryInterface(ScriptRegistryInterface const&) = delete;
    ScriptRegistryInterface(ScriptRegistryInterface&&) = delete;

    ScriptRegistryInterface& operator= (ScriptRegistryInterface const&) = delete;
    ScriptRegistryInterface& operator= (ScriptRegistryInterface&&) = delete;

    /// Removes all scripts associated with the given script context.
    /// Requires ScriptRegistryBase::SwapContext to be called after all transfers have finished.
    virtual void ReleaseContext(std::string const& context) = 0;

    /// Injects and updates the changed script objects.
    virtual void SwapContext(bool initialize) = 0;

    /// Removes the scripts used by this registry from the given container.
    /// Used to find unused script names.
    virtual void RemoveUsedScriptsFromContainer(std::unordered_set<std::string>& scripts) = 0;

    /// Unloads the script registry.
    virtual void Unload() = 0;
};

template<class>
class ScriptRegistry;

class ScriptRegistryCompositum
    : public ScriptRegistryInterface
{
    ScriptRegistryCompositum() { }

    template<class>
    friend class ScriptRegistry;

    /// Type erasure wrapper for objects
    class DeleteableObjectBase
    {
    public:
        DeleteableObjectBase() { }
        virtual ~DeleteableObjectBase() { }

        DeleteableObjectBase(DeleteableObjectBase const&) = delete;
        DeleteableObjectBase& operator= (DeleteableObjectBase const&) = delete;
    };

    template<typename T>
    class DeleteableObject
        : public DeleteableObjectBase
    {
    public:
        DeleteableObject(T&& object)
            : _object(std::forward<T>(object)) { }

    private:
        T _object;
    };

public:
    void SetScriptNameInContext(std::string const& scriptname, std::string const& context)
    {
        ASSERT(_scriptnames_to_context.find(scriptname) == _scriptnames_to_context.end(),
               "Scriptname was assigned to this context already!");
        _scriptnames_to_context.insert(std::make_pair(scriptname, context));
    }

    std::string const& GetScriptContextOfScriptName(std::string const& scriptname) const
    {
        auto itr = _scriptnames_to_context.find(scriptname);
        ASSERT(itr != _scriptnames_to_context.end() &&
               "Given scriptname doesn't exist!");
        return itr->second;
    }

    void ReleaseContext(std::string const& context) final override
    {
        for (auto const registry : _registries)
            registry->ReleaseContext(context);

        // Clear the script names in context after calling the release hooks
        // since it's possible that new references to a shared library
        // are acquired when releasing.
        for (auto itr = _scriptnames_to_context.begin();
                        itr != _scriptnames_to_context.end();)
            if (itr->second == context)
                itr = _scriptnames_to_context.erase(itr);
            else
                ++itr;
    }

    void SwapContext(bool initialize) final override
    {
        for (auto const registry : _registries)
            registry->SwapContext(initialize);

        DoDelayedDelete();
    }

    void RemoveUsedScriptsFromContainer(std::unordered_set<std::string>& scripts) final override
    {
        for (auto const registry : _registries)
            registry->RemoveUsedScriptsFromContainer(scripts);
    }

    void Unload() final override
    {
        for (auto const registry : _registries)
            registry->Unload();
    }

    template<typename T>
    void QueueForDelayedDelete(T&& any)
    {
        _delayed_delete_queue.push_back(
            Trinity::make_unique<
                DeleteableObject<typename std::decay<T>::type>
            >(std::forward<T>(any))
        );
    }

    static ScriptRegistryCompositum* Instance()
    {
        static ScriptRegistryCompositum instance;
        return &instance;
    }

private:
    void Register(ScriptRegistryInterface* registry)
    {
        _registries.insert(registry);
    }

    void DoDelayedDelete()
    {
        _delayed_delete_queue.clear();
    }

    std::unordered_set<ScriptRegistryInterface*> _registries;

    std::vector<std::unique_ptr<DeleteableObjectBase>> _delayed_delete_queue;

    std::unordered_map<
        std::string /*script name*/,
        std::string /*context*/
    > _scriptnames_to_context;
};

#define sScriptRegistryCompositum ScriptRegistryCompositum::Instance()

template<typename /*ScriptType*/, bool /*IsDatabaseBound*/>
class SpecializedScriptRegistry;

// This is the global static registry of scripts.
template<class ScriptType>
class ScriptRegistry final
    : public SpecializedScriptRegistry<
        ScriptType, is_script_database_bound<ScriptType>::value>
{
    ScriptRegistry()
    {
        sScriptRegistryCompositum->Register(this);
    }

public:
    static ScriptRegistry* Instance()
    {
        static ScriptRegistry instance;
        return &instance;
    }

    void LogDuplicatedScriptPointerError(ScriptType const* first, ScriptType const* second)
    {
        // See if the script is using the same memory as another script. If this happens, it means that
        // someone forgot to allocate new memory for a script.
        TC_LOG_ERROR("scripts", "Script '%s' has same memory pointer as '%s'.",
            first->GetName().c_str(), second->GetName().c_str());
    }
};

class ScriptRegistrySwapHookBase
{
public:
    ScriptRegistrySwapHookBase() { }
    virtual ~ScriptRegistrySwapHookBase() { }

    ScriptRegistrySwapHookBase(ScriptRegistrySwapHookBase const&) = delete;
    ScriptRegistrySwapHookBase(ScriptRegistrySwapHookBase&&) = delete;

    ScriptRegistrySwapHookBase& operator= (ScriptRegistrySwapHookBase const&) = delete;
    ScriptRegistrySwapHookBase& operator= (ScriptRegistrySwapHookBase&&) = delete;

    /// Called before the actual context release happens
    virtual void BeforeReleaseContext(std::string const& /*context*/) { }

    /// Called before SwapContext
    virtual void BeforeSwapContext(bool /*initialize*/) { }

    /// Called before Unload
    virtual void BeforeUnload() { }
};

template<typename ScriptType, typename Base>
class ScriptRegistrySwapHooks
    : public ScriptRegistrySwapHookBase
{
};

template<typename Base>
class UnsupportedScriptRegistrySwapHooks
    : public ScriptRegistrySwapHookBase
{
public:
    void BeforeReleaseContext(std::string const& context) final override
    {
        auto const bounds = static_cast<Base*>(this)->_ids_of_contexts.equal_range(context);
        ASSERT(bounds.first == bounds.second);
    }
};

/// This hook is responsible for swapping Creature, GameObject and AreaTrigger AI's
template<typename ObjectType, typename ScriptType, typename Base>
class CreatureGameObjectAreaTriggerScriptRegistrySwapHooks
    : public ScriptRegistrySwapHookBase
{
    template<typename W>
    class AIFunctionMapWorker
    {
    public:
        template<typename T>
        AIFunctionMapWorker(T&& worker)
            : _worker(std::forward<T>(worker)) { }

        void Visit(std::unordered_map<ObjectGuid, ObjectType*>& objects)
        {
            _worker(objects);
        }

        template<typename O>
        void Visit(std::unordered_map<ObjectGuid, O*>&) { }

    private:
        W _worker;
    };

    class AsyncCastHotswapEffectEvent : public BasicEvent
    {
    public:
        explicit AsyncCastHotswapEffectEvent(Unit* owner) : owner_(owner) { }

        bool Execute(uint64 /*e_time*/, uint32 /*p_time*/) override
        {
            owner_->CastSpell(owner_, SPELL_HOTSWAP_VISUAL_SPELL_EFFECT, true);
            return true;
        }

    private:
        Unit* owner_;
    };

    // Hook which is called before a creature is swapped
    static void UnloadResetScript(Creature* creature)
    {
        // Remove deletable events only,
        // otherwise it causes crashes with non-deletable spell events.
        creature->m_Events.KillAllEvents(false);

        if (creature->IsCharmed())
            creature->RemoveCharmedBy(nullptr);

        ASSERT(!creature->IsCharmed(),
               "There is a disabled AI which is still loaded.");

        if (creature->IsAlive())
            creature->AI()->EnterEvadeMode();
    }

    static void UnloadDestroyScript(Creature* creature)
    {
        bool const destroyed = creature->AIM_Destroy();
        ASSERT(destroyed,
               "Destroying the AI should never fail here!");
        (void)destroyed;

        ASSERT(!creature->AI(),
               "The AI should be null here!");
    }

    // Hook which is called before a gameobject is swapped
    static void UnloadResetScript(GameObject* gameobject)
    {
        gameobject->AI()->Reset();
    }

    static void UnloadDestroyScript(GameObject* gameobject)
    {
        gameobject->AIM_Destroy();

        ASSERT(!gameobject->AI(),
               "The AI should be null here!");
    }

    // Hook which is called before a areatrigger is swapped
    static void UnloadResetScript(AreaTrigger* at)
    {
        at->AI()->OnRemove();
    }

    static void UnloadDestroyScript(AreaTrigger* at)
    {
        at->AI_Destroy();

        ASSERT(!at->AI(),
            "The AI should be null here!");
    }

    // Hook which is called after a creature was swapped
    static void LoadInitializeScript(Creature* creature)
    {
        ASSERT(!creature->AI(),
               "The AI should be null here!");

        if (creature->IsAlive())
            creature->ClearUnitState(UNIT_STATE_EVADE);

        bool const created = creature->AIM_Create();
        ASSERT(created,
               "Creating the AI should never fail here!");
        (void)created;
    }

    static void LoadResetScript(Creature* creature)
    {
        if (!creature->IsAlive())
            return;

        creature->AI_InitializeAndEnable();
        creature->AI()->EnterEvadeMode();

        // Cast a dummy visual spell asynchronously here to signal
        // that the AI was hot swapped
        creature->m_Events.AddEvent(new AsyncCastHotswapEffectEvent(creature),
            creature->m_Events.CalculateTime(0));
    }

    // Hook which is called after a gameobject was swapped
    static void LoadInitializeScript(GameObject* gameobject)
    {
        ASSERT(!gameobject->AI(),
               "The AI should be null here!");

        gameobject->AIM_Initialize();
    }

    static void LoadResetScript(GameObject* gameobject)
    {
        gameobject->AI()->Reset();
    }

    // Hook which is called after a areatrigger was swapped
    static void LoadInitializeScript(AreaTrigger* at)
    {
        ASSERT(!at->AI(),
            "The AI should be null here!");

        at->AI_Initialize();
    }

    static void LoadResetScript(AreaTrigger* at)
    {
        at->AI()->OnCreate();
    }

    static Creature* GetEntityFromMap(std::common_type<Creature>, Map* map, ObjectGuid const& guid)
    {
        return map->GetCreature(guid);
    }

    static GameObject* GetEntityFromMap(std::common_type<GameObject>, Map* map, ObjectGuid const& guid)
    {
        return map->GetGameObject(guid);
    }

    static AreaTrigger* GetEntityFromMap(std::common_type<AreaTrigger>, Map* map, ObjectGuid const& guid)
    {
        return map->GetAreaTrigger(guid);
    }

    template<typename T>
    static void VisitObjectsToSwapOnMap(Map* map, std::unordered_set<uint32> const& idsToRemove, T visitor)
    {
        auto evaluator = [&](std::unordered_map<ObjectGuid, ObjectType*>& objects)
        {
            for (auto object : objects)
            {
                // When the script Id of the script isn't removed in this
                // context change, do nothing.
                if (idsToRemove.find(object.second->GetScriptId()) != idsToRemove.end())
                    visitor(object.second);
            }
        };

        AIFunctionMapWorker<typename std::decay<decltype(evaluator)>::type> worker(std::move(evaluator));
        TypeContainerVisitor<decltype(worker), MapStoredObjectTypesContainer> containerVisitor(worker);

        containerVisitor.Visit(map->GetObjectsStore());
    }

    static void DestroyScriptIdsFromSet(std::unordered_set<uint32> const& idsToRemove)
    {
        // First reset all swapped scripts safe by guid
        // Skip creatures and gameobjects with an empty guid
        // (that were not added to the world as of now)
        sMapMgr->DoForAllMaps([&](Map* map)
        {
            std::vector<ObjectGuid> guidsToReset;

            VisitObjectsToSwapOnMap(map, idsToRemove, [&](ObjectType* object)
            {
                if (object->AI() && !object->GetGUID().IsEmpty())
                    guidsToReset.push_back(object->GetGUID());
            });

            for (ObjectGuid const& guid : guidsToReset)
            {
                if (auto entity = GetEntityFromMap(std::common_type<ObjectType>{}, map, guid))
                    UnloadResetScript(entity);
            }

            VisitObjectsToSwapOnMap(map, idsToRemove, [&](ObjectType* object)
            {
                // Destroy the scripts instantly
                UnloadDestroyScript(object);
            });
        });
    }

    static void InitializeScriptIdsFromSet(std::unordered_set<uint32> const& idsToRemove)
    {
        sMapMgr->DoForAllMaps([&](Map* map)
        {
            std::vector<ObjectGuid> guidsToReset;

            VisitObjectsToSwapOnMap(map, idsToRemove, [&](ObjectType* object)
            {
                if (!object->AI() && !object->GetGUID().IsEmpty())
                {
                    // Initialize the script
                    LoadInitializeScript(object);
                    guidsToReset.push_back(object->GetGUID());
                }
            });

            for (ObjectGuid const& guid : guidsToReset)
            {
                // Reset the script
                if (auto entity = GetEntityFromMap(std::common_type<ObjectType>{}, map, guid))
                {
                    if (!entity->AI())
                        LoadInitializeScript(entity);

                    LoadResetScript(entity);
                }
            }
        });
    }

public:
    void BeforeReleaseContext(std::string const& context) final override
    {
        auto idsToRemove = static_cast<Base*>(this)->GetScriptIDsToRemove(context);
        DestroyScriptIdsFromSet(idsToRemove);

        // Add the new ids which are removed to the global ids to remove set
        ids_removed_.insert(idsToRemove.begin(), idsToRemove.end());
    }

    void BeforeSwapContext(bool initialize) override
    {
        // Never swap creature or gameobject scripts when initializing
        if (initialize)
            return;

        // Add the recently added scripts to the deleted scripts to replace
        // default AI's with recently added core scripts.
        ids_removed_.insert(static_cast<Base*>(this)->GetRecentlyAddedScriptIDs().begin(),
                            static_cast<Base*>(this)->GetRecentlyAddedScriptIDs().end());

        DestroyScriptIdsFromSet(ids_removed_);
        InitializeScriptIdsFromSet(ids_removed_);

        ids_removed_.clear();
    }

    void BeforeUnload() final override
    {
        ASSERT(ids_removed_.empty());
    }

private:
    std::unordered_set<uint32> ids_removed_;
};

// This hook is responsible for swapping CreatureAI's
template<typename Base>
class ScriptRegistrySwapHooks<CreatureScript, Base>
    : public CreatureGameObjectAreaTriggerScriptRegistrySwapHooks<
        Creature, CreatureScript, Base
      > { };

// This hook is responsible for swapping GameObjectAI's
template<typename Base>
class ScriptRegistrySwapHooks<GameObjectScript, Base>
    : public CreatureGameObjectAreaTriggerScriptRegistrySwapHooks<
        GameObject, GameObjectScript, Base
      > { };

// This hook is responsible for swapping AreaTriggerAI's
template<typename Base>
class ScriptRegistrySwapHooks<AreaTriggerEntityScript, Base>
    : public CreatureGameObjectAreaTriggerScriptRegistrySwapHooks<
    AreaTrigger, AreaTriggerEntityScript, Base
    > { };

/// This hook is responsible for swapping BattlegroundScript's
template<typename Base>
class ScriptRegistrySwapHooks<BattlegroundScript, Base>
    : public UnsupportedScriptRegistrySwapHooks<Base> { };

/// This hook is responsible for swapping Garrison's
template<typename Base>
class ScriptRegistrySwapHooks<GarrisonScript, Base>
    : public UnsupportedScriptRegistrySwapHooks<Base> { };

/// This hook is responsible for swapping OutdoorPvP's
template<typename Base>
class ScriptRegistrySwapHooks<OutdoorPvPScript, Base>
    : public ScriptRegistrySwapHookBase
{
public:
    ScriptRegistrySwapHooks() : swapped(false) { }

    void BeforeReleaseContext(std::string const& context) final override
    {
        auto const bounds = static_cast<Base*>(this)->_ids_of_contexts.equal_range(context);

        if ((!swapped) && (bounds.first != bounds.second))
        {
            swapped = true;
            sOutdoorPvPMgr->Die();
        }
    }

    void BeforeSwapContext(bool initialize) override
    {
        // Never swap outdoor pvp scripts when initializing
        if ((!initialize) && swapped)
        {
            sOutdoorPvPMgr->InitOutdoorPvP();
            swapped = false;
        }
    }

    void BeforeUnload() final override
    {
        ASSERT(!swapped);
    }

private:
    bool swapped;
};

/// This hook is responsible for swapping InstanceMapScript's
template<typename Base>
class ScriptRegistrySwapHooks<InstanceMapScript, Base>
    : public ScriptRegistrySwapHookBase
{
public:
    ScriptRegistrySwapHooks()  : swapped(false) { }

    void BeforeReleaseContext(std::string const& context) final override
    {
        auto const bounds = static_cast<Base*>(this)->_ids_of_contexts.equal_range(context);
        if (bounds.first != bounds.second)
            swapped = true;
    }

    void BeforeSwapContext(bool /*initialize*/) override
    {
        swapped = false;
    }

    void BeforeUnload() final override
    {
        ASSERT(!swapped);
    }

private:
    bool swapped;
};

/// This hook is responsible for swapping SceneScript's
template<typename Base>
class ScriptRegistrySwapHooks<SceneScript, Base>
    : public ScriptRegistrySwapHookBase
{
public:
    ScriptRegistrySwapHooks() : swapped(false) { }

    void BeforeReleaseContext(std::string const& context) final override
    {
        auto const bounds = static_cast<Base*>(this)->_ids_of_contexts.equal_range(context);
        if (bounds.first != bounds.second)
            swapped = true;
    }

    void BeforeSwapContext(bool /*initialize*/) override
    {
        swapped = false;
    }

    void BeforeUnload() final override
    {
        ASSERT(!swapped);
    }

private:
    bool swapped;
};

/// This hook is responsible for swapping QuestScript's
template<typename Base>
class ScriptRegistrySwapHooks<QuestScript, Base>
    : public ScriptRegistrySwapHookBase
{
public:
    ScriptRegistrySwapHooks() : swapped(false) { }

    void BeforeReleaseContext(std::string const& context) final override
    {
        auto const bounds = static_cast<Base*>(this)->_ids_of_contexts.equal_range(context);
        if (bounds.first != bounds.second)
            swapped = true;
    }

    void BeforeSwapContext(bool /*initialize*/) override
    {
        swapped = false;
    }

    void BeforeUnload() final override
    {
        ASSERT(!swapped);
    }

private:
    bool swapped;
};

/// This hook is responsible for swapping ZoneScript's
template<typename Base>
class ScriptRegistrySwapHooks<ZoneScript, Base>
    : public ScriptRegistrySwapHookBase
{
public:
    ScriptRegistrySwapHooks() : swapped(false) { }

    void BeforeReleaseContext(std::string const& context) final override
    {
        auto const bounds = static_cast<Base*>(this)->_ids_of_contexts.equal_range(context);
        if (bounds.first != bounds.second)
            swapped = true;
    }

    void BeforeSwapContext(bool /*initialize*/) override
    {
        swapped = false;
    }

    void BeforeUnload() final override
    {
        ASSERT(!swapped);
    }

private:
    bool swapped;
};

/// This hook is responsible for swapping SpellScriptLoader's
template<typename Base>
class ScriptRegistrySwapHooks<SpellScriptLoader, Base>
    : public ScriptRegistrySwapHookBase
{
public:
    ScriptRegistrySwapHooks() : swapped(false) { }

    void BeforeReleaseContext(std::string const& context) final override
    {
        auto const bounds = static_cast<Base*>(this)->_ids_of_contexts.equal_range(context);

        if (bounds.first != bounds.second)
            swapped = true;
    }

    void BeforeSwapContext(bool /*initialize*/) override
    {
        if (swapped)
        {
            sObjectMgr->ValidateSpellScripts();
            swapped = false;
        }
    }

    void BeforeUnload() final override
    {
        ASSERT(!swapped);
    }

private:
    bool swapped;
};

// Database bound script registry
template<typename ScriptType>
class SpecializedScriptRegistry<ScriptType, true>
    : public ScriptRegistryInterface,
      public ScriptRegistrySwapHooks<ScriptType, ScriptRegistry<ScriptType>>
{
    template<typename>
    friend class UnsupportedScriptRegistrySwapHooks;

    template<typename, typename>
    friend class ScriptRegistrySwapHooks;

    template<typename, typename, typename>
    friend class CreatureGameObjectAreaTriggerScriptRegistrySwapHooks;

public:
    SpecializedScriptRegistry() { }

    typedef std::unordered_map<
        uint32 /*script id*/,
        std::unique_ptr<ScriptType>
    > ScriptStoreType;

    typedef typename ScriptStoreType::iterator ScriptStoreIteratorType;

    void ReleaseContext(std::string const& context) final override
    {
        this->BeforeReleaseContext(context);

        auto const bounds = _ids_of_contexts.equal_range(context);
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
            _scripts.erase(itr->second);
    }

    void SwapContext(bool initialize) final override
    {
      this->BeforeSwapContext(initialize);

      _recently_added_ids.clear();
    }

    void RemoveUsedScriptsFromContainer(std::unordered_set<std::string>& scripts) final override
    {
        for (auto const& script : _scripts)
            scripts.erase(script.second->GetName());
    }

    void Unload() final override
    {
        this->BeforeUnload();

        ASSERT(_recently_added_ids.empty(),
               "Recently added script ids should be empty here!");

        _scripts.clear();
        _ids_of_contexts.clear();
    }

    // Adds a database bound script
    void AddScript(ScriptType* script)
    {
        ASSERT(script,
               "Tried to call AddScript with a nullpointer!");
        ASSERT(!sScriptMgr->GetCurrentScriptContext().empty(),
               "Tried to register a script without being in a valid script context!");

        std::unique_ptr<ScriptType> script_ptr(script);

        // Get an ID for the script. An ID only exists if it's a script that is assigned in the database
        // through a script name (or similar).
        if (uint32 const id = sObjectMgr->GetScriptId(script->GetName()))
        {
            // Try to find an existing script.
            for (auto const& stored_script : _scripts)
            {
                // If the script names match...
                if (stored_script.second->GetName() == script->GetName())
                {
                    // If the script is already assigned -> delete it!
                    TC_LOG_ERROR("scripts", "Script '%s' already assigned with the same script name, "
                        "so the script can't work.", script->GetName().c_str());

                    // Error that should be fixed ASAP.
                    sScriptRegistryCompositum->QueueForDelayedDelete(std::move(script_ptr));
                    ABORT();
                    return;
                }
            }

            // If the script isn't assigned -> assign it!
            _scripts.insert(std::make_pair(id, std::move(script_ptr)));
            _ids_of_contexts.insert(std::make_pair(sScriptMgr->GetCurrentScriptContext(), id));
            _recently_added_ids.insert(id);

            sScriptRegistryCompositum->SetScriptNameInContext(script->GetName(),
                sScriptMgr->GetCurrentScriptContext());
        }
        else
        {
            // The script uses a script name from database, but isn't assigned to anything.
            TC_LOG_ERROR("sql.sql", "Script named '%s' does not have a script name assigned in database.",
                script->GetName().c_str());

            // Avoid calling "delete script;" because we are currently in the script constructor
            // In a valid scenario this will not happen because every script has a name assigned in the database
            sScriptRegistryCompositum->QueueForDelayedDelete(std::move(script_ptr));
            return;
        }
    }

    // Gets a script by its ID (assigned by ObjectMgr).
    ScriptType* GetScriptById(uint32 id)
    {
        auto const itr = _scripts.find(id);
        if (itr != _scripts.end())
            return itr->second.get();

        return nullptr;
    }

    ScriptStoreType& GetScripts()
    {
        return _scripts;
    }

protected:
    // Returns the script id's which are registered to a certain context
    std::unordered_set<uint32> GetScriptIDsToRemove(std::string const& context) const
    {
        // Create a set of all ids which are removed
        std::unordered_set<uint32> scripts_to_remove;

        auto const bounds = _ids_of_contexts.equal_range(context);
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
            scripts_to_remove.insert(itr->second);

        return scripts_to_remove;
    }

    std::unordered_set<uint32> const& GetRecentlyAddedScriptIDs() const
    {
        return _recently_added_ids;
    }

private:
    ScriptStoreType _scripts;

    // Scripts of a specific context
    std::unordered_multimap<std::string /*context*/, uint32 /*id*/> _ids_of_contexts;

    // Script id's which were registered recently
    std::unordered_set<uint32> _recently_added_ids;
};

/// This hook is responsible for swapping CommandScript's
template<typename Base>
class ScriptRegistrySwapHooks<CommandScript, Base>
    : public ScriptRegistrySwapHookBase
{
public:
    void BeforeReleaseContext(std::string const& /*context*/) final override
    {
        ChatHandler::invalidateCommandTable();
    }

    void BeforeSwapContext(bool /*initialize*/) override
    {
        ChatHandler::invalidateCommandTable();
    }

    void BeforeUnload() final override
    {
        ChatHandler::invalidateCommandTable();
    }
};

// Database unbound script registry
template<typename ScriptType>
class SpecializedScriptRegistry<ScriptType, false>
    : public ScriptRegistryInterface,
      public ScriptRegistrySwapHooks<ScriptType, ScriptRegistry<ScriptType>>
{
    template<typename, typename>
    friend class ScriptRegistrySwapHooks;

public:
    typedef std::unordered_multimap<std::string /*context*/, std::unique_ptr<ScriptType>> ScriptStoreType;
    typedef typename ScriptStoreType::iterator ScriptStoreIteratorType;

    SpecializedScriptRegistry() { }

    void ReleaseContext(std::string const& context) final override
    {
        this->BeforeReleaseContext(context);

        _scripts.erase(context);
    }

    void SwapContext(bool initialize) final override
    {
        this->BeforeSwapContext(initialize);
    }

    void RemoveUsedScriptsFromContainer(std::unordered_set<std::string>& scripts) final override
    {
        for (auto const& script : _scripts)
            scripts.erase(script.second->GetName());
    }

    void Unload() final override
    {
        this->BeforeUnload();

        _scripts.clear();
    }

    // Adds a non database bound script
    void AddScript(ScriptType* script)
    {
        ASSERT(script,
               "Tried to call AddScript with a nullpointer!");
        ASSERT(!sScriptMgr->GetCurrentScriptContext().empty(),
               "Tried to register a script without being in a valid script context!");

        std::unique_ptr<ScriptType> script_ptr(script);

        for (auto const& entry : _scripts)
            if (entry.second.get() == script)
            {
                static_cast<ScriptRegistry<ScriptType>*>(this)->
                    LogDuplicatedScriptPointerError(script, entry.second.get());

                sScriptRegistryCompositum->QueueForDelayedDelete(std::move(script_ptr));
                return;
            }

        // We're dealing with a code-only script, just add it.
        _scripts.insert(std::make_pair(sScriptMgr->GetCurrentScriptContext(), std::move(script_ptr)));
    }

    ScriptStoreType& GetScripts()
    {
        return _scripts;
    }

private:
    ScriptStoreType _scripts;
};

// Utility macros to refer to the script registry.
#define SCR_REG_MAP(T) ScriptRegistry<T>::ScriptStoreType
#define SCR_REG_ITR(T) ScriptRegistry<T>::ScriptStoreIteratorType
#define SCR_REG_LST(T) ScriptRegistry<T>::Instance()->GetScripts()

// Utility macros for looping over scripts.
#define FOR_SCRIPTS(T, C, E) \
    if (!SCR_REG_LST(T).empty()) \
        for (SCR_REG_ITR(T) C = SCR_REG_LST(T).begin(); \
            C != SCR_REG_LST(T).end(); ++C)

#define FOR_SCRIPTS_RET(T, C, E, R) \
    if (SCR_REG_LST(T).empty()) \
        return R; \
    \
    for (SCR_REG_ITR(T) C = SCR_REG_LST(T).begin(); \
        C != SCR_REG_LST(T).end(); ++C)

#define FOREACH_SCRIPT(T) \
    FOR_SCRIPTS(T, itr, end) \
        itr->second

// Utility macros for finding specific scripts.
#define GET_SCRIPT_NO_RET(T, I, V) \
    T* V = ScriptRegistry<T>::Instance()->GetScriptById(I);

#define GET_SCRIPT(T, I, V) \
    T* V = ScriptRegistry<T>::Instance()->GetScriptById(I); \
    if (!V) \
        return;

#define GET_SCRIPT_RET(T, I, V, R) \
    T* V = ScriptRegistry<T>::Instance()->GetScriptById(I); \
    if (!V) \
        return R;

struct TSpellSummary
{
    uint8 Targets;                                          // set of enum SelectTarget
    uint8 Effects;                                          // set of enum SelectEffect
} *SpellSummary;

ScriptObject::ScriptObject(const char* name) : _name(name)
{
    sScriptMgr->IncreaseScriptCount();
}

ScriptObject::~ScriptObject()
{
    sScriptMgr->DecreaseScriptCount();
}

ScriptMgr::ScriptMgr()
  : _scriptCount(0), _script_loader_callback(nullptr)
{
}

ScriptMgr::~ScriptMgr() { }

ScriptMgr* ScriptMgr::instance()
{
    static ScriptMgr instance;
    return &instance;
}

void ScriptMgr::Initialize()
{
    ASSERT(sSpellMgr->GetSpellInfo(SPELL_HOTSWAP_VISUAL_SPELL_EFFECT)
           && "Reload hotswap spell effect for creatures isn't valid!");

    uint32 oldMSTime = getMSTime();

    LoadDatabase();

    TC_LOG_INFO("server.loading", "Loading C++ scripts");

    FillSpellSummary();

    // Load core scripts
    SetScriptContext(GetNameOfStaticContext());

    // SmartAI
    AddSC_SmartScripts();

    // LFGScripts
    lfg::AddSC_LFGScripts();

    // Load all static linked scripts through the script loader function.
    ASSERT(_script_loader_callback,
           "Script loader callback wasn't registered!");
    _script_loader_callback();

    // Initialize all dynamic scripts
    // and finishes the context switch to do
    // bulk loading
    sScriptReloadMgr->Initialize();

    // Loads all scripts from the current context
    sScriptMgr->SwapScriptContext(true);

    // Print unused script names.
    std::unordered_set<std::string> unusedScriptNames(
        sObjectMgr->GetAllScriptNames().begin(),
        sObjectMgr->GetAllScriptNames().end());

    // Remove the used scripts from the given container.
    sScriptRegistryCompositum->RemoveUsedScriptsFromContainer(unusedScriptNames);

    for (std::string const& scriptName : unusedScriptNames)
    {
        // Avoid complaining about empty script names since the
        // script name container contains a placeholder as the 0 element.
        if (scriptName.empty())
            continue;

        TC_LOG_ERROR("sql.sql", "ScriptName '%s' exists in database, "
                     "but no core script found!", scriptName.c_str());
    }

    TC_LOG_INFO("server.loading", ">> Loaded %u C++ scripts in %u ms",
        GetScriptCount(), GetMSTimeDiffToNow(oldMSTime));
}

void ScriptMgr::SetScriptContext(std::string const& context)
{
    _currentContext = context;
}

void ScriptMgr::SwapScriptContext(bool initialize)
{
    sScriptRegistryCompositum->SwapContext(initialize);
    _currentContext.clear();
}

std::string const& ScriptMgr::GetNameOfStaticContext()
{
    static std::string const name = "___static___";
    return name;
}

void ScriptMgr::ReleaseScriptContext(std::string const& context)
{
    sScriptRegistryCompositum->ReleaseContext(context);
}

std::shared_ptr<ModuleReference>
    ScriptMgr::AcquireModuleReferenceOfScriptName(std::string const& scriptname) const
{
#ifdef TRINITY_API_USE_DYNAMIC_LINKING
    // Returns the reference to the module of the given scriptname
    return ScriptReloadMgr::AcquireModuleReferenceOfContext(
        sScriptRegistryCompositum->GetScriptContextOfScriptName(scriptname));
#else
    (void)scriptname;
    // Something went wrong when this function is used in
    // a static linked context.
    WPAbort();
#endif // #ifndef TRINITY_API_USE_DYNAMIC_LINKING
}

void ScriptMgr::Unload()
{
    sScriptRegistryCompositum->Unload();

    delete[] SpellSummary;
    delete[] UnitAI::AISpellInfo;
}

void ScriptMgr::LoadDatabase()
{
    sScriptSystemMgr->LoadScriptWaypoints();
    sScriptSystemMgr->LoadScriptSplineChains();
}

void ScriptMgr::FillSpellSummary()
{
    UnitAI::FillAISpellInfo();

    SpellSummary = new TSpellSummary[sSpellMgr->GetSpellInfoStoreSize()];

    SpellInfo const* pTempSpell;

    for (uint32 i = 0; i < sSpellMgr->GetSpellInfoStoreSize(); ++i)
    {
        SpellSummary[i].Effects = 0;
        SpellSummary[i].Targets = 0;

        pTempSpell = sSpellMgr->GetSpellInfo(i);
        // This spell doesn't exist.
        if (!pTempSpell)
            continue;

        for (SpellEffectInfo const* effect : pTempSpell->GetEffectsForDifficulty(DIFFICULTY_NONE))
        {
            if (!effect)
                continue;

            // Spell targets self.
            if (effect->TargetA.GetTarget() == TARGET_UNIT_CASTER)
                SpellSummary[i].Targets |= 1 << (SELECT_TARGET_SELF-1);

            // Spell targets a single enemy.
            if (effect->TargetA.GetTarget() == TARGET_UNIT_TARGET_ENEMY ||
                effect->TargetA.GetTarget() == TARGET_DEST_TARGET_ENEMY)
                SpellSummary[i].Targets |= 1 << (SELECT_TARGET_SINGLE_ENEMY-1);

            // Spell targets AoE at enemy.
            if (effect->TargetA.GetTarget() == TARGET_UNIT_SRC_AREA_ENEMY ||
                effect->TargetA.GetTarget() == TARGET_UNIT_DEST_AREA_ENEMY ||
                effect->TargetA.GetTarget() == TARGET_SRC_CASTER ||
                effect->TargetA.GetTarget() == TARGET_DEST_DYNOBJ_ENEMY)
                SpellSummary[i].Targets |= 1 << (SELECT_TARGET_AOE_ENEMY-1);

            // Spell targets an enemy.
            if (effect->TargetA.GetTarget() == TARGET_UNIT_TARGET_ENEMY ||
                effect->TargetA.GetTarget() == TARGET_DEST_TARGET_ENEMY ||
                effect->TargetA.GetTarget() == TARGET_UNIT_SRC_AREA_ENEMY ||
                effect->TargetA.GetTarget() == TARGET_UNIT_DEST_AREA_ENEMY ||
                effect->TargetA.GetTarget() == TARGET_SRC_CASTER ||
                effect->TargetA.GetTarget() == TARGET_DEST_DYNOBJ_ENEMY)
                SpellSummary[i].Targets |= 1 << (SELECT_TARGET_ANY_ENEMY-1);

            // Spell targets a single friend (or self).
            if (effect->TargetA.GetTarget() == TARGET_UNIT_CASTER ||
                effect->TargetA.GetTarget() == TARGET_UNIT_TARGET_ALLY ||
                effect->TargetA.GetTarget() == TARGET_UNIT_TARGET_PARTY)
                SpellSummary[i].Targets |= 1 << (SELECT_TARGET_SINGLE_FRIEND-1);

            // Spell targets AoE friends.
            if (effect->TargetA.GetTarget() == TARGET_UNIT_CASTER_AREA_PARTY ||
                effect->TargetA.GetTarget() == TARGET_UNIT_LASTTARGET_AREA_PARTY ||
                effect->TargetA.GetTarget() == TARGET_SRC_CASTER)
                SpellSummary[i].Targets |= 1 << (SELECT_TARGET_AOE_FRIEND-1);

            // Spell targets any friend (or self).
            if (effect->TargetA.GetTarget() == TARGET_UNIT_CASTER ||
                effect->TargetA.GetTarget() == TARGET_UNIT_TARGET_ALLY ||
                effect->TargetA.GetTarget() == TARGET_UNIT_TARGET_PARTY ||
                effect->TargetA.GetTarget() == TARGET_UNIT_CASTER_AREA_PARTY ||
                effect->TargetA.GetTarget() == TARGET_UNIT_LASTTARGET_AREA_PARTY ||
                effect->TargetA.GetTarget() == TARGET_SRC_CASTER)
                SpellSummary[i].Targets |= 1 << (SELECT_TARGET_ANY_FRIEND-1);

            // Make sure that this spell includes a damage effect.
            if (effect->Effect == SPELL_EFFECT_SCHOOL_DAMAGE ||
                effect->Effect == SPELL_EFFECT_INSTAKILL ||
                effect->Effect == SPELL_EFFECT_ENVIRONMENTAL_DAMAGE ||
                effect->Effect == SPELL_EFFECT_HEALTH_LEECH)
                SpellSummary[i].Effects |= 1 << (SELECT_EFFECT_DAMAGE-1);

            // Make sure that this spell includes a healing effect (or an apply aura with a periodic heal).
            if (effect->Effect == SPELL_EFFECT_HEAL ||
                effect->Effect == SPELL_EFFECT_HEAL_MAX_HEALTH ||
                effect->Effect == SPELL_EFFECT_HEAL_MECHANICAL ||
                (effect->Effect == SPELL_EFFECT_APPLY_AURA  && effect->ApplyAuraName == 8))
                SpellSummary[i].Effects |= 1 << (SELECT_EFFECT_HEALING-1);

            // Make sure that this spell applies an aura.
            if (effect->Effect == SPELL_EFFECT_APPLY_AURA)
                SpellSummary[i].Effects |= 1 << (SELECT_EFFECT_AURA-1);
        }
    }
}

template<typename T, typename F, typename O>
void CreateSpellOrAuraScripts(uint32 spellId, std::vector<T*>& scriptVector, F&& extractor, O* objectInvoker)
{
    SpellScriptsBounds bounds = sObjectMgr->GetSpellScriptsBounds(spellId);
    for (auto itr = bounds.first; itr != bounds.second; ++itr)
    {
        // When the script is disabled continue with the next one
        if (!itr->second.second)
            continue;

        SpellScriptLoader* tmpscript = sScriptMgr->GetSpellScriptLoader(itr->second.first);
        if (!tmpscript)
            continue;

        T* script = (*tmpscript.*extractor)();
        if (!script)
            continue;

        script->_Init(&tmpscript->GetName(), spellId);
        if (!script->_Load(objectInvoker))
        {
            delete script;
            continue;
        }

        scriptVector.push_back(script);
    }
}

void ScriptMgr::CreateSpellScripts(uint32 spellId, std::vector<SpellScript*>& scriptVector, Spell* invoker) const
{
    CreateSpellOrAuraScripts(spellId, scriptVector, &SpellScriptLoader::GetSpellScript, invoker);
}

void ScriptMgr::CreateAuraScripts(uint32 spellId, std::vector<AuraScript*>& scriptVector, Aura* invoker) const
{
    CreateSpellOrAuraScripts(spellId, scriptVector, &SpellScriptLoader::GetAuraScript, invoker);
}

SpellScriptLoader* ScriptMgr::GetSpellScriptLoader(uint32 scriptId)
{
    return ScriptRegistry<SpellScriptLoader>::Instance()->GetScriptById(scriptId);
}

void ScriptMgr::OnNetworkStart()
{
    FOREACH_SCRIPT(ServerScript)->OnNetworkStart();
}

void ScriptMgr::OnNetworkStop()
{
    FOREACH_SCRIPT(ServerScript)->OnNetworkStop();
}

void ScriptMgr::OnSocketOpen(std::shared_ptr<WorldSocket> socket)
{
    ASSERT(socket);

    FOREACH_SCRIPT(ServerScript)->OnSocketOpen(socket);
}

void ScriptMgr::OnSocketClose(std::shared_ptr<WorldSocket> socket)
{
    ASSERT(socket);

    FOREACH_SCRIPT(ServerScript)->OnSocketClose(socket);
}

void ScriptMgr::OnPacketReceive(WorldSession* session, WorldPacket const& packet)
{
    if (SCR_REG_LST(ServerScript).empty())
        return;

    WorldPacket copy(packet);
    FOREACH_SCRIPT(ServerScript)->OnPacketReceive(session, copy);
}

void ScriptMgr::OnPacketSend(WorldSession* session, WorldPacket const& packet)
{
    ASSERT(session);

    if (SCR_REG_LST(ServerScript).empty())
        return;

    WorldPacket copy(packet);
    FOREACH_SCRIPT(ServerScript)->OnPacketSend(session, copy);
}

void ScriptMgr::OnOpenStateChange(bool open)
{
    FOREACH_SCRIPT(WorldScript)->OnOpenStateChange(open);
}

void ScriptMgr::OnConfigLoad(bool reload)
{
    FOREACH_SCRIPT(WorldScript)->OnConfigLoad(reload);
}

void ScriptMgr::OnMotdChange(std::string& newMotd)
{
    FOREACH_SCRIPT(WorldScript)->OnMotdChange(newMotd);
}

void ScriptMgr::OnShutdownInitiate(ShutdownExitCode code, ShutdownMask mask)
{
    FOREACH_SCRIPT(WorldScript)->OnShutdownInitiate(code, mask);
}

void ScriptMgr::OnShutdownCancel()
{
    FOREACH_SCRIPT(WorldScript)->OnShutdownCancel();
}

void ScriptMgr::OnWorldUpdate(uint32 diff)
{
    FOREACH_SCRIPT(WorldScript)->OnUpdate(diff);
}

void ScriptMgr::OnBeforeCreateCharacter(WorldSession* me,bool& failed)
{
    FOREACH_SCRIPT(WorldScript)->OnBeforeCreateCharacter(me, failed);
}
void ScriptMgr::OnHonorCalculation(float& honor, uint8 level, float multiplier)
{
    FOREACH_SCRIPT(FormulaScript)->OnHonorCalculation(honor, level, multiplier);
}

void ScriptMgr::OnGrayLevelCalculation(uint8& grayLevel, uint8 playerLevel)
{
    FOREACH_SCRIPT(FormulaScript)->OnGrayLevelCalculation(grayLevel, playerLevel);
}

void ScriptMgr::OnColorCodeCalculation(XPColorChar& color, uint8 playerLevel, uint8 mobLevel)
{
    FOREACH_SCRIPT(FormulaScript)->OnColorCodeCalculation(color, playerLevel, mobLevel);
}

void ScriptMgr::OnZeroDifferenceCalculation(uint8& diff, uint8 playerLevel)
{
    FOREACH_SCRIPT(FormulaScript)->OnZeroDifferenceCalculation(diff, playerLevel);
}

void ScriptMgr::OnBaseGainCalculation(uint32& gain, uint8 playerLevel, uint8 mobLevel)
{
    FOREACH_SCRIPT(FormulaScript)->OnBaseGainCalculation(gain, playerLevel, mobLevel);
}

void ScriptMgr::OnGainCalculation(uint32& gain, Player* player, Unit* unit)
{
    ASSERT(player);
    ASSERT(unit);

    FOREACH_SCRIPT(FormulaScript)->OnGainCalculation(gain, player, unit);
}

void ScriptMgr::OnGroupRateCalculation(float& rate, uint32 count, bool isRaid)
{
    FOREACH_SCRIPT(FormulaScript)->OnGroupRateCalculation(rate, count, isRaid);
}

void ScriptMgr::OnTalentCalculation(Player const* player, uint32& result, uint32 m_questRewardTalentCount)
{
    FOREACH_SCRIPT(FormulaScript)->OnTalentCalculation(player, result, m_questRewardTalentCount);
}

void ScriptMgr::OnStatToAttackPowerCalculation(Player const* player, float level, float& val2, bool ranged)
{
    FOREACH_SCRIPT(FormulaScript)->OnStatToAttackPowerCalculation( player, level,  val2, ranged);
}

void ScriptMgr::OnSpellBaseDamageBonusDone(Player const* player, int32& DoneAdvertisedBenefit)
{
    FOREACH_SCRIPT(FormulaScript)->OnSpellBaseDamageBonusDone(player,DoneAdvertisedBenefit);
}

void ScriptMgr::OnSpellBaseHealingBonusDone(Player const* player, int32& DoneAdvertisedBenefit)
{
    FOREACH_SCRIPT(FormulaScript)->OnSpellBaseHealingBonusDone(player,DoneAdvertisedBenefit);
}
void ScriptMgr::OnUpdateResistance(Player const* player, uint32 school, float& value)
{
    FOREACH_SCRIPT(FormulaScript)->OnUpdateResistance(player, school,  value);
}
void ScriptMgr::OnUpdateArmor(Player const* player, float& value)
{
    FOREACH_SCRIPT(FormulaScript)->OnUpdateArmor( player,  value);
}
void ScriptMgr::OnManaRestore(Player* player, float& addvalue)
{
    FOREACH_SCRIPT(FormulaScript)->OnManaRestore( player,  addvalue);
}
void ScriptMgr::OnHealthRestore(Player* player, float& addvalue, uint32 HealthIncreaseRate)
{
    FOREACH_SCRIPT(FormulaScript)->OnHealthRestore( player,  addvalue, HealthIncreaseRate);
}

void ScriptMgr::OnCanRollForItemInLFG(Player const* player, InventoryResult& RETURN_CODE, ItemTemplate const* proto)
{
    FOREACH_SCRIPT(FormulaScript)->OnCanRollForItemInLFG( player,  RETURN_CODE,  proto);
}

void  ScriptMgr::OnQuestXPValue(uint32 playerlevel, uint32& xp, int32 Level, uint32 RewardXPMultiplier, uint32 RewardXPDifficulty, uint32 GetQuestMaxScalingLevel)
{
    FOREACH_SCRIPT(FormulaScript)->OnQuestXPValue(playerlevel,  xp,  Level, RewardXPMultiplier, RewardXPDifficulty, GetQuestMaxScalingLevel);
}

void ScriptMgr::OnCreatureDazePlayer(CalcDamageInfo* damageInfo, Unit* attacker, Unit* victim, float& Probability)
{
    FOREACH_SCRIPT(FormulaScript)->OnCreatureDazePlayer( damageInfo,  attacker,  victim, Probability);
}

void ScriptMgr::OnArmorLevelPenaltyCalculation(Unit const* attacker, Unit const* victim, SpellInfo const* spellInfo, uint8 attackerLevel, float& armor, float& tmpvalue)
{
    FOREACH_SCRIPT(FormulaScript)->OnArmorLevelPenaltyCalculation( attacker,  victim,  spellInfo, attackerLevel,  armor,  tmpvalue);
}

void ScriptMgr::OnResistChanceCalculation(Unit const * me,  Unit* victim, SpellSchoolMask schoolMask, SpellInfo const* spellInfo, uint32& resistance)
{
    FOREACH_SCRIPT(FormulaScript)->OnResistChanceCalculation(me,   victim, schoolMask, spellInfo, resistance);
}

void ScriptMgr::OnRollMeleeOutcomeAgainst(bool& SkipOtherCode,Unit const* me, Unit const* victim, WeaponAttackType attType, MeleeHitOutcome& result)
{
    FOREACH_SCRIPT(FormulaScript)->OnRollMeleeOutcomeAgainst(SkipOtherCode, me, victim, attType, result);
}

void ScriptMgr::OnMeleeSpellMissChance(Unit const* me, const Unit* victim, WeaponAttackType attType, uint32 spellId, float& missChance)
{
    FOREACH_SCRIPT(FormulaScript)->OnMeleeSpellMissChance(me, victim, attType, spellId, missChance);
}

void ScriptMgr::OnMeleeSpellHitResult(bool& SkipOtherCode,Unit const* me, Unit* victim, SpellInfo const*  spellInfo, SpellMissInfo& result)
{
    FOREACH_SCRIPT(FormulaScript)->OnMeleeSpellHitResult(SkipOtherCode, me, victim, spellInfo, result);
}

void ScriptMgr::OnAgroRange(Creature const* me, Unit const* target,float& aggroRadius)
{
    FOREACH_SCRIPT(FormulaScript)->OnAgroRange(me, target, aggroRadius);
}

void ScriptMgr::OnStealthDetectLevelCalculate(WorldObject const* me, WorldObject const* obj, int32& detectionValue, bool owner)
{
    FOREACH_SCRIPT(FormulaScript)->OnStealthDetectLevelCalculate(me, obj, detectionValue, owner);
}

void ScriptMgr::OnCalculateMeleeDamage(Unit* me,Unit* victim,uint32 damage, WeaponAttackType attackType, CalcDamageInfo* damageInfo)
{
    FOREACH_SCRIPT(FormulaScript)->OnCalculateMeleeDamage(me, victim, damage, attackType, damageInfo);
}

void ScriptMgr::OnUpdateCraftSkill(Player* me, uint32 spelllevel, uint32 SkillId, uint32 craft_skill_gain, bool& result)
{
    FOREACH_SCRIPT(FormulaScript)->OnUpdateCraftSkill( me, spelllevel, SkillId, craft_skill_gain,  result);
}

void ScriptMgr::OnMagicSpellHitLevelCalculate(Unit const* me, Unit* victim, SpellInfo const* spell, int32& modHitChance)
{
    FOREACH_SCRIPT(FormulaScript)->OnMagicSpellHitLevelCalculate(me, victim, spell, modHitChance);
}

void ScriptMgr::OnUpdateChances(Player* me, uint32 ChanceType, float& value)
{
    FOREACH_SCRIPT(FormulaScript)->OnUpdateChances(me, ChanceType, value);
}

void ScriptMgr::OnBuildPlayerLevelInfo(uint8 race,uint8 _class,uint8 level, PlayerLevelInfo* info)
{
    FOREACH_SCRIPT(FormulaScript)->OnBuildPlayerLevelInfo(race, _class, level, info);
}

void ScriptMgr::UpdatePotionCooldown(Player* me, Spell* spell, uint32& m_lastPotionId,bool& SkipOtherCode)
{
    FOREACH_SCRIPT(FormulaScript)->UpdatePotionCooldown(me, spell, m_lastPotionId, SkipOtherCode);
}

void ScriptMgr::OnInitTalentForLevel(Player* me, bool& SkipOtherCode)
{
    FOREACH_SCRIPT(FormulaScript)->OnInitTalentForLevel(me,  SkipOtherCode);
}

void ScriptMgr::OnInitTaxiNodesForLevel(uint32 race, uint32 chrClass, uint8 level, PlayerTaxi* me, bool& SkipOtherCode)
{
    FOREACH_SCRIPT(FormulaScript)->OnInitTaxiNodesForLevel(race, chrClass, level, me,  SkipOtherCode);
}

void ScriptMgr::OnCalculateMinMaxDamage(Player* me, WeaponAttackType attType, bool normalized, bool addTotalPct, float& minDamage, float& maxDamage,bool& SkipOtherCode)
{
    FOREACH_SCRIPT(FormulaScript)->OnCalculateMinMaxDamage(me, attType, normalized, addTotalPct, minDamage, maxDamage, SkipOtherCode);
}

void ScriptMgr::OnCanUseItem(Player const* me, Item* pItem, bool not_loading, InventoryResult& result, bool& SkipOtherCode)
{
    FOREACH_SCRIPT(FormulaScript)->OnCanUseItem(me, pItem, not_loading, result, SkipOtherCode);
}

void ScriptMgr::OnCallAssistance(Creature* me,bool m_AlreadyCallAssistance, EventProcessor& m_Events, bool& SkipOtherCode)
{
    FOREACH_SCRIPT(FormulaScript)->OnCallAssistance(me, m_AlreadyCallAssistance, m_Events, SkipOtherCode);
}

void ScriptMgr::OnDurabilityLoss(Player* me, Item* item, double percent, bool& SkipOtherCode)
{
    FOREACH_SCRIPT(FormulaScript)->OnDurabilityLoss( me,  item,  percent,  SkipOtherCode);
}

void ScriptMgr::OnIsPrimaryProfessionSkill(SkillLineEntry const* pSkill, uint32 SkillId, bool& result)
{
    FOREACH_SCRIPT(FormulaScript)->OnIsPrimaryProfessionSkill( pSkill,  SkillId,  result);
}
#define SCR_MAP_BGN(M, V, I, E, C, T) \
    if (V->GetEntry() && V->GetEntry()->T()) \
    { \
        FOR_SCRIPTS(M, I, E) \
        { \
            MapEntry const* C = I->second->GetEntry(); \
            if (!C) \
                continue; \
            if (C->ID == V->GetId()) \
            {

#define SCR_MAP_END \
                return; \
            } \
        } \
    }

void ScriptMgr::OnCreateMap(Map* map)
{
    ASSERT(map);

    SCR_MAP_BGN(WorldMapScript, map, itr, end, entry, IsWorldMap);
        itr->second->OnCreate(map);
    SCR_MAP_END;

    SCR_MAP_BGN(InstanceMapScript, map, itr, end, entry, IsDungeon);
        itr->second->OnCreate((InstanceMap*)map);
    SCR_MAP_END;

    SCR_MAP_BGN(BattlegroundMapScript, map, itr, end, entry, IsBattleground);
        itr->second->OnCreate((BattlegroundMap*)map);
    SCR_MAP_END;
}

void ScriptMgr::OnDestroyMap(Map* map)
{
    ASSERT(map);

    SCR_MAP_BGN(WorldMapScript, map, itr, end, entry, IsWorldMap);
        itr->second->OnDestroy(map);
    SCR_MAP_END;

    SCR_MAP_BGN(InstanceMapScript, map, itr, end, entry, IsDungeon);
        itr->second->OnDestroy((InstanceMap*)map);
    SCR_MAP_END;

    SCR_MAP_BGN(BattlegroundMapScript, map, itr, end, entry, IsBattleground);
        itr->second->OnDestroy((BattlegroundMap*)map);
    SCR_MAP_END;
}

void ScriptMgr::OnLoadGridMap(Map* map, GridMap* gmap, uint32 gx, uint32 gy)
{
    ASSERT(map);
    ASSERT(gmap);

    SCR_MAP_BGN(WorldMapScript, map, itr, end, entry, IsWorldMap);
        itr->second->OnLoadGridMap(map, gmap, gx, gy);
    SCR_MAP_END;

    SCR_MAP_BGN(InstanceMapScript, map, itr, end, entry, IsDungeon);
        itr->second->OnLoadGridMap((InstanceMap*)map, gmap, gx, gy);
    SCR_MAP_END;

    SCR_MAP_BGN(BattlegroundMapScript, map, itr, end, entry, IsBattleground);
        itr->second->OnLoadGridMap((BattlegroundMap*)map, gmap, gx, gy);
    SCR_MAP_END;
}

void ScriptMgr::OnUnloadGridMap(Map* map, GridMap* gmap, uint32 gx, uint32 gy)
{
    ASSERT(map);
    ASSERT(gmap);

    SCR_MAP_BGN(WorldMapScript, map, itr, end, entry, IsWorldMap);
        itr->second->OnUnloadGridMap(map, gmap, gx, gy);
    SCR_MAP_END;

    SCR_MAP_BGN(InstanceMapScript, map, itr, end, entry, IsDungeon);
        itr->second->OnUnloadGridMap((InstanceMap*)map, gmap, gx, gy);
    SCR_MAP_END;

    SCR_MAP_BGN(BattlegroundMapScript, map, itr, end, entry, IsBattleground);
        itr->second->OnUnloadGridMap((BattlegroundMap*)map, gmap, gx, gy);
    SCR_MAP_END;
}

void ScriptMgr::OnPlayerEnterMap(Map* map, Player* player)
{
    ASSERT(map);
    ASSERT(player);

    FOREACH_SCRIPT(PlayerScript)->OnMapChanged(player);

    SCR_MAP_BGN(WorldMapScript, map, itr, end, entry, IsWorldMap);
        itr->second->OnPlayerEnter(map, player);
    SCR_MAP_END;

    SCR_MAP_BGN(InstanceMapScript, map, itr, end, entry, IsDungeon);
        itr->second->OnPlayerEnter((InstanceMap*)map, player);
    SCR_MAP_END;

    SCR_MAP_BGN(BattlegroundMapScript, map, itr, end, entry, IsBattleground);
        itr->second->OnPlayerEnter((BattlegroundMap*)map, player);
    SCR_MAP_END;
}

void ScriptMgr::OnPlayerLeaveMap(Map* map, Player* player)
{
    ASSERT(map);
    ASSERT(player);

    SCR_MAP_BGN(WorldMapScript, map, itr, end, entry, IsWorldMap);
        itr->second->OnPlayerLeave(map, player);
    SCR_MAP_END;

    SCR_MAP_BGN(InstanceMapScript, map, itr, end, entry, IsDungeon);
        itr->second->OnPlayerLeave((InstanceMap*)map, player);
    SCR_MAP_END;

    SCR_MAP_BGN(BattlegroundMapScript, map, itr, end, entry, IsBattleground);
        itr->second->OnPlayerLeave((BattlegroundMap*)map, player);
    SCR_MAP_END;
}

void ScriptMgr::OnMapUpdate(Map* map, uint32 diff)
{
    ASSERT(map);

    SCR_MAP_BGN(WorldMapScript, map, itr, end, entry, IsWorldMap);
        itr->second->OnUpdate(map, diff);
    SCR_MAP_END;

    SCR_MAP_BGN(InstanceMapScript, map, itr, end, entry, IsDungeon);
        itr->second->OnUpdate((InstanceMap*)map, diff);
    SCR_MAP_END;

    SCR_MAP_BGN(BattlegroundMapScript, map, itr, end, entry, IsBattleground);
        itr->second->OnUpdate((BattlegroundMap*)map, diff);
    SCR_MAP_END;
}

#undef SCR_MAP_BGN
#undef SCR_MAP_END

InstanceScript* ScriptMgr::CreateInstanceData(InstanceMap* map)
{
    ASSERT(map);

    GET_SCRIPT_RET(InstanceMapScript, map->GetScriptId(), tmpscript, NULL);
    return tmpscript->GetInstanceScript(map);
}

bool ScriptMgr::OnDummyEffect(Unit* caster, uint32 spellId, SpellEffIndex effIndex, Item* target)
{
    ASSERT(caster);
    ASSERT(target);

    GET_SCRIPT_RET(ItemScript, target->GetScriptId(), tmpscript, false);
    return tmpscript->OnDummyEffect(caster, spellId, effIndex, target);
}

bool ScriptMgr::OnQuestAccept(Player* player, Item* item, Quest const* quest)
{
    ASSERT(player);
    ASSERT(item);
    ASSERT(quest);

    GET_SCRIPT_RET(ItemScript, item->GetScriptId(), tmpscript, false);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->OnQuestAccept(player, item, quest);
}

bool ScriptMgr::OnItemUse(Player* player, Item* item, SpellCastTargets const& targets, ObjectGuid castId)
{
    ASSERT(player);
    ASSERT(item);

    GET_SCRIPT_RET(ItemScript, item->GetScriptId(), tmpscript, false);
    return tmpscript->OnUse(player, item, targets, castId);
}

bool ScriptMgr::OnItemOpen(Player* player, Item* item)
{
    ASSERT(player);
    ASSERT(item);

    GET_SCRIPT_RET(ItemScript, item->GetScriptId(), tmpscript, false);
    return tmpscript->OnOpen(player, item);
}

bool ScriptMgr::OnItemExpire(Player* player, ItemTemplate const* proto)
{
    ASSERT(player);
    ASSERT(proto);

    GET_SCRIPT_RET(ItemScript, proto->ScriptId, tmpscript, false);
    return tmpscript->OnExpire(player, proto);
}

bool ScriptMgr::OnItemRemove(Player* player, Item* item)
{
    ASSERT(player);
    ASSERT(item);

    GET_SCRIPT_RET(ItemScript, item->GetScriptId(), tmpscript, false);
    return tmpscript->OnRemove(player, item);
}

void ScriptMgr::OnGossipSelect(Player* player, Item* item, uint32 sender, uint32 action)
{
    ASSERT(player);
    ASSERT(item);

    GET_SCRIPT(ItemScript, item->GetScriptId(), tmpscript);
    tmpscript->OnGossipSelect(player, item, sender, action);
}

void ScriptMgr::OnGossipSelectCode(Player* player, Item* item, uint32 sender, uint32 action, const char* code)
{
    ASSERT(player);
    ASSERT(item);

    GET_SCRIPT(ItemScript, item->GetScriptId(), tmpscript);
    tmpscript->OnGossipSelectCode(player, item, sender, action, code);
}

bool ScriptMgr::OnDummyEffect(Unit* caster, uint32 spellId, SpellEffIndex effIndex, Creature* target)
{
    ASSERT(caster);
    ASSERT(target);

    GET_SCRIPT_RET(CreatureScript, target->GetScriptId(), tmpscript, false);
    return tmpscript->OnDummyEffect(caster, spellId, effIndex, target);
}

bool ScriptMgr::OnGossipHello(Player* player, Creature* creature)
{
    ASSERT(player);
    ASSERT(creature);

    GET_SCRIPT_RET(CreatureScript, creature->GetScriptId(), tmpscript, false);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->OnGossipHello(player, creature);
}

bool ScriptMgr::OnGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action)
{
    ASSERT(player);
    ASSERT(creature);

    GET_SCRIPT_RET(CreatureScript, creature->GetScriptId(), tmpscript, false);
    return tmpscript->OnGossipSelect(player, creature, sender, action);
}

bool ScriptMgr::OnGossipSelectCode(Player* player, Creature* creature, uint32 sender, uint32 action, const char* code)
{
    ASSERT(player);
    ASSERT(creature);
    ASSERT(code);

    GET_SCRIPT_RET(CreatureScript, creature->GetScriptId(), tmpscript, false);
    return tmpscript->OnGossipSelectCode(player, creature, sender, action, code);
}

bool ScriptMgr::OnQuestAccept(Player* player, Creature* creature, Quest const* quest)
{
    ASSERT(player);
    ASSERT(creature);
    ASSERT(quest);

    GET_SCRIPT_RET(CreatureScript, creature->GetScriptId(), tmpscript, false);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->OnQuestAccept(player, creature, quest);
}

bool ScriptMgr::OnQuestSelect(Player* player, Creature* creature, Quest const* quest)
{
    ASSERT(player);
    ASSERT(creature);
    ASSERT(quest);

    GET_SCRIPT_RET(CreatureScript, creature->GetScriptId(), tmpscript, false);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->OnQuestSelect(player, creature, quest);
}

bool ScriptMgr::OnQuestReward(Player* player, Creature* creature, Quest const* quest, uint32 opt)
{
    ASSERT(player);
    ASSERT(creature);
    ASSERT(quest);

    GET_SCRIPT_RET(CreatureScript, creature->GetScriptId(), tmpscript, false);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->OnQuestReward(player, creature, quest, opt);
}

uint32 ScriptMgr::GetDialogStatus(Player* player, Creature* creature)
{
    ASSERT(player);
    ASSERT(creature);

    GET_SCRIPT_RET(CreatureScript, creature->GetScriptId(), tmpscript, DIALOG_STATUS_SCRIPTED_NO_STATUS);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->GetDialogStatus(player, creature);
}

bool ScriptMgr::CanSpawn(ObjectGuid::LowType spawnId, uint32 entry, CreatureTemplate const* actTemplate, CreatureData const* cData, Map const* map)
{
    ASSERT(actTemplate);

    CreatureTemplate const* baseTemplate = sObjectMgr->GetCreatureTemplate(entry);
    if (!baseTemplate)
        baseTemplate = actTemplate;
    GET_SCRIPT_RET(CreatureScript, (cData ? cData->ScriptId : baseTemplate->ScriptID), tmpscript, true);
    return tmpscript->CanSpawn(spawnId, entry, baseTemplate, actTemplate, cData, map);
}

CreatureAI* ScriptMgr::GetCreatureAI(Creature* creature)
{
    ASSERT(creature);

    GET_SCRIPT_NO_RET(CreatureScript, creature->GetScriptId(), tmpscript);
    if (tmpscript)
        return tmpscript->GetAI(creature);

    GET_SCRIPT_NO_RET(VehicleScript, creature->GetScriptId(), tmpVehiclescript);
    if (tmpVehiclescript)
        return tmpVehiclescript->GetAI(creature);

    return nullptr;
}

GameObjectAI* ScriptMgr::GetGameObjectAI(GameObject* gameobject)
{
    ASSERT(gameobject);

    GET_SCRIPT_RET(GameObjectScript, gameobject->GetScriptId(), tmpscript, nullptr);
    return tmpscript->GetAI(gameobject);
}

AreaTriggerAI* ScriptMgr::GetAreaTriggerAI(AreaTrigger* areatrigger)
{
    ASSERT(areatrigger);

    GET_SCRIPT_RET(AreaTriggerEntityScript, areatrigger->GetScriptId(), tmpscript, nullptr);
    return tmpscript->GetAI(areatrigger);
}

GarrisonAI* ScriptMgr::GetGarrisonAI(Garrison* garrison)
{
    ASSERT(garrison);

    GET_SCRIPT_RET(GarrisonScript, garrison->GetScriptId(), tmpscript, nullptr);
    return tmpscript->GetAI(garrison);
}

ZoneScript* ScriptMgr::GetZoneScript(uint32 scriptId)
{
    if (!scriptId)
        return nullptr;

    GET_SCRIPT_RET(ZoneScript, scriptId, zoneScript, nullptr);
    return zoneScript;
}

void ScriptMgr::OnCreatureUpdate(Creature* creature, uint32 diff)
{
    ASSERT(creature);

    FOREACH_SCRIPT(AllCreatureScript)->OnAllCreatureUpdate(creature, diff);

    GET_SCRIPT(CreatureScript, creature->GetScriptId(), tmpscript);
    tmpscript->OnUpdate(creature, diff);
}

bool ScriptMgr::OnGossipHello(Player* player, GameObject* go)
{
    ASSERT(player);
    ASSERT(go);

    GET_SCRIPT_RET(GameObjectScript, go->GetScriptId(), tmpscript, false);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->OnGossipHello(player, go);
}

bool ScriptMgr::OnGossipSelect(Player* player, GameObject* go, uint32 sender, uint32 action)
{
    ASSERT(player);
    ASSERT(go);

    GET_SCRIPT_RET(GameObjectScript, go->GetScriptId(), tmpscript, false);
    return tmpscript->OnGossipSelect(player, go, sender, action);
}

bool ScriptMgr::OnGossipSelectCode(Player* player, GameObject* go, uint32 sender, uint32 action, const char* code)
{
    ASSERT(player);
    ASSERT(go);
    ASSERT(code);

    GET_SCRIPT_RET(GameObjectScript, go->GetScriptId(), tmpscript, false);
    return tmpscript->OnGossipSelectCode(player, go, sender, action, code);
}

bool ScriptMgr::OnQuestAccept(Player* player, GameObject* go, Quest const* quest)
{
    ASSERT(player);
    ASSERT(go);
    ASSERT(quest);

    GET_SCRIPT_RET(GameObjectScript, go->GetScriptId(), tmpscript, false);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->OnQuestAccept(player, go, quest);
}

bool ScriptMgr::OnQuestReward(Player* player, GameObject* go, Quest const* quest, uint32 opt)
{
    ASSERT(player);
    ASSERT(go);
    ASSERT(quest);

    GET_SCRIPT_RET(GameObjectScript, go->GetScriptId(), tmpscript, false);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->OnQuestReward(player, go, quest, opt);
}

uint32 ScriptMgr::GetDialogStatus(Player* player, GameObject* go)
{
    ASSERT(player);
    ASSERT(go);

    GET_SCRIPT_RET(GameObjectScript, go->GetScriptId(), tmpscript, DIALOG_STATUS_SCRIPTED_NO_STATUS);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->GetDialogStatus(player, go);
}

void ScriptMgr::OnGameObjectDestroyed(GameObject* go, Player* player)
{
    ASSERT(go);

    GET_SCRIPT(GameObjectScript, go->GetScriptId(), tmpscript);
    tmpscript->OnDestroyed(go, player);
}

void ScriptMgr::OnGameObjectDamaged(GameObject* go, Player* player)
{
    ASSERT(go);

    GET_SCRIPT(GameObjectScript, go->GetScriptId(), tmpscript);
    tmpscript->OnDamaged(go, player);
}

void ScriptMgr::OnGameObjectLootStateChanged(GameObject* go, uint32 state, Unit* unit)
{
    ASSERT(go);

    GET_SCRIPT(GameObjectScript, go->GetScriptId(), tmpscript);
    tmpscript->OnLootStateChanged(go, state, unit);
}

void ScriptMgr::OnGameObjectStateChanged(GameObject* go, uint32 state)
{
    ASSERT(go);

    GET_SCRIPT(GameObjectScript, go->GetScriptId(), tmpscript);
    tmpscript->OnGameObjectStateChanged(go, state);
}

void ScriptMgr::OnGameObjectUpdate(GameObject* go, uint32 diff)
{
    ASSERT(go);

    GET_SCRIPT(GameObjectScript, go->GetScriptId(), tmpscript);
    tmpscript->OnUpdate(go, diff);
}

bool ScriptMgr::OnDummyEffect(Unit* caster, uint32 spellId, SpellEffIndex effIndex, GameObject* target)
{
    ASSERT(caster);
    ASSERT(target);

    GET_SCRIPT_RET(GameObjectScript, target->GetScriptId(), tmpscript, false);
    return tmpscript->OnDummyEffect(caster, spellId, effIndex, target);
}

bool ScriptMgr::OnAreaTrigger(Player* player, AreaTriggerEntry const* trigger, bool entered)
{
    ASSERT(player);
    ASSERT(trigger);

    GET_SCRIPT_RET(AreaTriggerScript, sObjectMgr->GetAreaTriggerScriptId(trigger->ID), tmpscript, false);
    return tmpscript->OnTrigger(player, trigger, entered);
}

Battleground* ScriptMgr::CreateBattleground(BattlegroundTypeId /*typeId*/)
{
    /// @todo Implement script-side battlegrounds.
    ABORT();
    return NULL;
}

OutdoorPvP* ScriptMgr::CreateOutdoorPvP(OutdoorPvPData const* data)
{
    ASSERT(data);

    GET_SCRIPT_RET(OutdoorPvPScript, data->ScriptId, tmpscript, NULL);
    return tmpscript->GetOutdoorPvP();
}

std::vector<ChatCommand> ScriptMgr::GetChatCommands()
{
    std::vector<ChatCommand> table;

    FOR_SCRIPTS_RET(CommandScript, itr, end, table)
    {
        std::vector<ChatCommand> cmds = itr->second->GetCommands();
        table.insert(table.end(), cmds.begin(), cmds.end());
    }

    // Sort commands in alphabetical order
    std::sort(table.begin(), table.end(), [](const ChatCommand& a, const ChatCommand&b)
    {
        return strcmp(a.Name, b.Name) < 0;
    });

    return table;
}

void ScriptMgr::OnWeatherChange(Weather* weather, WeatherState state, float grade)
{
    ASSERT(weather);

    GET_SCRIPT(WeatherScript, weather->GetScriptId(), tmpscript);
    tmpscript->OnChange(weather, state, grade);
}

void ScriptMgr::OnWeatherUpdate(Weather* weather, uint32 diff)
{
    ASSERT(weather);

    GET_SCRIPT(WeatherScript, weather->GetScriptId(), tmpscript);
    tmpscript->OnUpdate(weather, diff);
}

void ScriptMgr::OnAuctionAdd(AuctionHouseObject* ah, AuctionEntry* entry)
{
    ASSERT(ah);
    ASSERT(entry);

    FOREACH_SCRIPT(AuctionHouseScript)->OnAuctionAdd(ah, entry);
}

void ScriptMgr::OnAuctionRemove(AuctionHouseObject* ah, AuctionEntry* entry)
{
    ASSERT(ah);
    ASSERT(entry);

    FOREACH_SCRIPT(AuctionHouseScript)->OnAuctionRemove(ah, entry);
}

void ScriptMgr::OnAuctionSuccessful(AuctionHouseObject* ah, AuctionEntry* entry)
{
    ASSERT(ah);
    ASSERT(entry);

    FOREACH_SCRIPT(AuctionHouseScript)->OnAuctionSuccessful(ah, entry);
}

void ScriptMgr::OnAuctionExpire(AuctionHouseObject* ah, AuctionEntry* entry)
{
    ASSERT(ah);
    ASSERT(entry);

    FOREACH_SCRIPT(AuctionHouseScript)->OnAuctionExpire(ah, entry);
}

bool ScriptMgr::OnConditionCheck(Condition const* condition, ConditionSourceInfo& sourceInfo)
{
    ASSERT(condition);

    GET_SCRIPT_RET(ConditionScript, condition->ScriptId, tmpscript, true);
    return tmpscript->OnConditionCheck(condition, sourceInfo);
}

void ScriptMgr::OnInstall(Vehicle* veh)
{
    ASSERT(veh);
    ASSERT(veh->GetBase()->GetTypeId() == TYPEID_UNIT);

    GET_SCRIPT(VehicleScript, veh->GetBase()->ToCreature()->GetScriptId(), tmpscript);
    tmpscript->OnInstall(veh);
}

void ScriptMgr::OnUninstall(Vehicle* veh)
{
    ASSERT(veh);
    ASSERT(veh->GetBase()->GetTypeId() == TYPEID_UNIT);

    GET_SCRIPT(VehicleScript, veh->GetBase()->ToCreature()->GetScriptId(), tmpscript);
    tmpscript->OnUninstall(veh);
}

void ScriptMgr::OnReset(Vehicle* veh)
{
    ASSERT(veh);
    ASSERT(veh->GetBase()->GetTypeId() == TYPEID_UNIT);

    GET_SCRIPT(VehicleScript, veh->GetBase()->ToCreature()->GetScriptId(), tmpscript);
    tmpscript->OnReset(veh);
}

void ScriptMgr::OnInstallAccessory(Vehicle* veh, Creature* accessory)
{
    ASSERT(veh);
    ASSERT(veh->GetBase()->GetTypeId() == TYPEID_UNIT);
    ASSERT(accessory);

    GET_SCRIPT(VehicleScript, veh->GetBase()->ToCreature()->GetScriptId(), tmpscript);
    tmpscript->OnInstallAccessory(veh, accessory);
}

void ScriptMgr::OnAddPassenger(Vehicle* veh, Unit* passenger, int8 seatId)
{
    ASSERT(veh);
    ASSERT(veh->GetBase()->GetTypeId() == TYPEID_UNIT);
    ASSERT(passenger);

    GET_SCRIPT(VehicleScript, veh->GetBase()->ToCreature()->GetScriptId(), tmpscript);
    tmpscript->OnAddPassenger(veh, passenger, seatId);
}

void ScriptMgr::OnRemovePassenger(Vehicle* veh, Unit* passenger)
{
    ASSERT(veh);
    ASSERT(veh->GetBase()->GetTypeId() == TYPEID_UNIT);
    ASSERT(passenger);

    GET_SCRIPT(VehicleScript, veh->GetBase()->ToCreature()->GetScriptId(), tmpscript);
    tmpscript->OnRemovePassenger(veh, passenger);
}

void ScriptMgr::OnDynamicObjectUpdate(DynamicObject* dynobj, uint32 diff)
{
    ASSERT(dynobj);

    FOR_SCRIPTS(DynamicObjectScript, itr, end)
        itr->second->OnUpdate(dynobj, diff);
}

void ScriptMgr::OnAddPassenger(Transport* transport, Player* player)
{
    ASSERT(transport);
    ASSERT(player);

    GET_SCRIPT(TransportScript, transport->GetScriptId(), tmpscript);
    tmpscript->OnAddPassenger(transport, player);
}

void ScriptMgr::OnAddCreaturePassenger(Transport* transport, Creature* creature)
{
    ASSERT(transport);
    ASSERT(creature);

    GET_SCRIPT(TransportScript, transport->GetScriptId(), tmpscript);
    tmpscript->OnAddCreaturePassenger(transport, creature);
}

void ScriptMgr::OnRemovePassenger(Transport* transport, Player* player)
{
    ASSERT(transport);
    ASSERT(player);

    GET_SCRIPT(TransportScript, transport->GetScriptId(), tmpscript);
    tmpscript->OnRemovePassenger(transport, player);
}

void ScriptMgr::OnTransportUpdate(Transport* transport, uint32 diff)
{
    ASSERT(transport);

    GET_SCRIPT(TransportScript, transport->GetScriptId(), tmpscript);
    tmpscript->OnUpdate(transport, diff);
}

void ScriptMgr::OnRelocate(Transport* transport, uint32 waypointId, uint32 mapId, float x, float y, float z)
{
    GET_SCRIPT(TransportScript, transport->GetScriptId(), tmpscript);
    tmpscript->OnRelocate(transport, waypointId, mapId, x, y, z);
}

void ScriptMgr::OnStartup()
{
    FOREACH_SCRIPT(WorldScript)->OnStartup();
}

void ScriptMgr::OnShutdown()
{
    FOREACH_SCRIPT(WorldScript)->OnShutdown();
}

bool ScriptMgr::OnCriteriaCheck(uint32 scriptId, Player* source, Unit* target)
{
    ASSERT(source);
    // target can be NULL.

    GET_SCRIPT_RET(AchievementCriteriaScript, scriptId, tmpscript, false);
    return tmpscript->OnCheck(source, target);
}

// Player
void ScriptMgr::OnPVPKill(Player* killer, Player* killed)
{
    FOREACH_SCRIPT(PlayerScript)->OnPVPKill(killer, killed);
}

void ScriptMgr::OnCreatureKill(Player* killer, Creature* killed)
{
    FOREACH_SCRIPT(PlayerScript)->OnCreatureKill(killer, killed);
}

void ScriptMgr::OnPlayerKilledByCreature(Creature* killer, Player* killed)
{
    FOREACH_SCRIPT(PlayerScript)->OnPlayerKilledByCreature(killer, killed);
}

void ScriptMgr::OnPlayerDeath(Player* player)
{
    FOREACH_SCRIPT(PlayerScript)->OnDeath(player);
}

void ScriptMgr::OnPlayerLevelChanged(Player* player, uint8 oldLevel)
{
    FOREACH_SCRIPT(PlayerScript)->OnLevelChanged(player, oldLevel);
}

void ScriptMgr::OnPlayerFreeTalentPointsChanged(Player* player, uint32 points)
{
    FOREACH_SCRIPT(PlayerScript)->OnFreeTalentPointsChanged(player, points);
}

void ScriptMgr::OnPlayerTalentsReset(Player* player, bool noCost)
{
    FOREACH_SCRIPT(PlayerScript)->OnTalentsReset(player, noCost);
}

void ScriptMgr::OnPlayerMoneyChanged(Player* player, int64& amount)
{
    FOREACH_SCRIPT(PlayerScript)->OnMoneyChanged(player, amount);
}

void ScriptMgr::OnPlayerMoneyLimit(Player* player, int64 amount)
{
    FOREACH_SCRIPT(PlayerScript)->OnMoneyLimit(player, amount);
}

void ScriptMgr::OnGivePlayerXP(Player* player, uint32& amount, Unit* victim)
{
    FOREACH_SCRIPT(PlayerScript)->OnGiveXP(player, amount, victim);
}

void ScriptMgr::OnPlayerReputationChange(Player* player, uint32 factionID, int32& standing, bool incremental)
{
    FOREACH_SCRIPT(PlayerScript)->OnReputationChange(player, factionID, standing, incremental);
}

void ScriptMgr::OnPlayerDuelRequest(Player* target, Player* challenger)
{
    FOREACH_SCRIPT(PlayerScript)->OnDuelRequest(target, challenger);
}

void ScriptMgr::OnPlayerDuelStart(Player* player1, Player* player2)
{
    FOREACH_SCRIPT(PlayerScript)->OnDuelStart(player1, player2);
}

void ScriptMgr::OnPlayerDuelEnd(Player* winner, Player* loser, DuelCompleteType type)
{
    FOREACH_SCRIPT(PlayerScript)->OnDuelEnd(winner, loser, type);
}

void ScriptMgr::OnPlayerChat(Player* player, uint32 type, uint32 lang, std::string& msg)
{
    FOREACH_SCRIPT(PlayerScript)->OnChat(player, type, lang, msg);
}

void ScriptMgr::OnPlayerChat(Player* player, uint32 type, uint32 lang, std::string& msg, Player* receiver)
{
    FOREACH_SCRIPT(PlayerScript)->OnChat(player, type, lang, msg, receiver);
}

void ScriptMgr::OnPlayerChat(Player* player, uint32 type, uint32 lang, std::string& msg, Group* group)
{
    FOREACH_SCRIPT(PlayerScript)->OnChat(player, type, lang, msg, group);
}

void ScriptMgr::OnPlayerChat(Player* player, uint32 type, uint32 lang, std::string& msg, Guild* guild)
{
    FOREACH_SCRIPT(PlayerScript)->OnChat(player, type, lang, msg, guild);
}

void ScriptMgr::OnPlayerChat(Player* player, uint32 type, uint32 lang, std::string& msg, Channel* channel)
{
    FOREACH_SCRIPT(PlayerScript)->OnChat(player, type, lang, msg, channel);
}

void ScriptMgr::OnPlayerClearEmote(Player* player)
{
    FOREACH_SCRIPT(PlayerScript)->OnClearEmote(player);
}

void ScriptMgr::OnPlayerTextEmote(Player* player, uint32 textEmote, uint32 emoteNum, ObjectGuid guid)
{
    FOREACH_SCRIPT(PlayerScript)->OnTextEmote(player, textEmote, emoteNum, guid);
}

void ScriptMgr::OnPlayerSpellCast(Player* player, Spell* spell, bool skipCheck)
{
    FOREACH_SCRIPT(PlayerScript)->OnSpellCast(player, spell, skipCheck);
}

void ScriptMgr::OnPlayerSuccessfulSpellCast(Player* player, Spell* spell)
{
    FOREACH_SCRIPT(PlayerScript)->OnSuccessfulSpellCast(player, spell);
}

void ScriptMgr::OnPlayerLogin(Player* player, bool firstLogin)
{
    FOREACH_SCRIPT(PlayerScript)->OnLogin(player, firstLogin);
}

void ScriptMgr::OnPlayerUpdate(Player* player, uint32 diff)
{
    FOREACH_SCRIPT(PlayerScript)->OnUpdate(player, diff);
}

void ScriptMgr::OnPlayerLogout(Player* player)
{
    FOREACH_SCRIPT(PlayerScript)->OnLogout(player);
}

void ScriptMgr::OnPlayerCreate(Player* player)
{
    FOREACH_SCRIPT(PlayerScript)->OnCreate(player);
}

void ScriptMgr::OnPlayerDelete(ObjectGuid guid, uint32 accountId)
{
    FOREACH_SCRIPT(PlayerScript)->OnDelete(guid, accountId);
}

void ScriptMgr::OnPlayerFailedDelete(ObjectGuid guid, uint32 accountId)
{
    FOREACH_SCRIPT(PlayerScript)->OnFailedDelete(guid, accountId);
}

void ScriptMgr::OnPlayerSave(Player* player)
{
    FOREACH_SCRIPT(PlayerScript)->OnSave(player);
}

void ScriptMgr::OnPlayerBindToInstance(Player* player, Difficulty difficulty, uint32 mapid, bool permanent, uint8 extendState)
{
    FOREACH_SCRIPT(PlayerScript)->OnBindToInstance(player, difficulty, mapid, permanent, extendState);
}

void ScriptMgr::OnPlayerUpdateZone(Player* player, Area* newArea, Area* oldArea)
{
    FOREACH_SCRIPT(PlayerScript)->OnUpdateZone(player, newArea, oldArea);
}

void ScriptMgr::OnPlayerUpdateArea(Player* player, Area* newArea, Area* oldArea)
{
    FOREACH_SCRIPT(PlayerScript)->OnUpdateArea(player, newArea, oldArea);
}

void ScriptMgr::OnPlayerUpdateAreaAlternate(Player* player, uint32 newArea, uint32 oldArea)
{
    FOREACH_SCRIPT(PlayerScript)->OnUpdateAreaAlternate(player, newArea, oldArea);
}

 void ScriptMgr::OnGossipSelect(Player* player, uint32 menu_id, uint32 sender, uint32 action)
 {
     FOREACH_SCRIPT(PlayerScript)->OnGossipSelect(player, menu_id, sender, action);
 }
 
 void ScriptMgr::OnGossipSelectCode(Player* player, uint32 menu_id, uint32 sender, uint32 action, const char* code)
 {
     FOREACH_SCRIPT(PlayerScript)->OnGossipSelectCode(player, menu_id, sender, action, code);
 }
 
void ScriptMgr::OnQuestAccept(Player* player, const Quest* quest)
{
    FOREACH_SCRIPT(PlayerScript)->OnQuestAccept(player, quest);
}

void ScriptMgr::OnQuestReward(Player* player, const Quest* quest)
{
    FOREACH_SCRIPT(PlayerScript)->OnQuestReward(player, quest);
}

void ScriptMgr::OnObjectiveValidate(Player* player, uint32 questID, uint32 objectiveID)
{
    FOREACH_SCRIPT(PlayerScript)->OnObjectiveValidate(player, questID, objectiveID);
}

void ScriptMgr::OnQuestComplete(Player* player, const Quest* quest)
{
    FOREACH_SCRIPT(PlayerScript)->OnQuestComplete(player, quest);
}

void ScriptMgr::OnQuestAbandon(Player* player, const Quest* quest)
{
    FOREACH_SCRIPT(PlayerScript)->OnQuestAbandon(player, quest);
}

void ScriptMgr::OnQuestStatusChange(Player* player, uint32 questId)
{
    FOREACH_SCRIPT(PlayerScript)->OnQuestStatusChange(player, questId);
}

void ScriptMgr::OnModifyPower(Player* player, Powers power, int32 oldValue, int32& newValue, bool regen, bool after)
{
    FOREACH_SCRIPT(PlayerScript)->OnModifyPower(player, power, oldValue, newValue, regen, after);
}

void ScriptMgr::OnPlayerTakeDamage(Player* player, uint32 damage, SpellSchoolMask schoolMask)
{
    FOREACH_SCRIPT(PlayerScript)->OnTakeDamage(player, damage, schoolMask);
}

void ScriptMgr::OnSceneStart(Player* player, uint32 scenePackageId, uint32 sceneInstanceId)
{
    FOREACH_SCRIPT(PlayerScript)->OnSceneStart(player, scenePackageId, sceneInstanceId);
}

void ScriptMgr::OnSceneTriggerEvent(Player* player, uint32 sceneInstanceId, std::string p_Event)
{
    FOREACH_SCRIPT(PlayerScript)->OnSceneTriggerEvent(player, sceneInstanceId, p_Event);
}

void ScriptMgr::OnSceneCancel(Player* player, uint32 sceneInstanceId)
{
    FOREACH_SCRIPT(PlayerScript)->OnSceneCancel(player, sceneInstanceId);
}

void ScriptMgr::OnSceneComplete(Player* player, uint32 sceneInstanceId)
{
    FOREACH_SCRIPT(PlayerScript)->OnSceneComplete(player, sceneInstanceId);
}

void ScriptMgr::OnMovieComplete(Player* player, uint32 movieId)
{
    FOREACH_SCRIPT(PlayerScript)->OnMovieComplete(player, movieId);
}

void ScriptMgr::OnPlayerChoiceResponse(Player* player, uint32 choiceId, uint32 responseId)
{
    FOREACH_SCRIPT(PlayerScript)->OnPlayerChoiceResponse(player, choiceId, responseId);
}

void ScriptMgr::OnCooldownStart(Player* player, SpellInfo const* spellInfo, uint32 itemId, int32& cooldown, uint32& categoryId, int32& categoryCooldown)
{
    FOREACH_SCRIPT(PlayerScript)->OnCooldownStart(player, spellInfo, itemId, cooldown, categoryId, categoryCooldown);
}

void ScriptMgr::OnChargeRecoveryTimeStart(Player* player, uint32 chargeCategoryId, int32& chargeRecoveryTime)
{
    FOREACH_SCRIPT(PlayerScript)->OnChargeRecoveryTimeStart(player, chargeCategoryId, chargeRecoveryTime);
}

void ScriptMgr::OnCreateItem(Player* player, Item* pItem, uint32 num_to_add, bool& success)
{
    FOREACH_SCRIPT(PlayerScript)->OnCreateItem(player, pItem, num_to_add, success);
}

void ScriptMgr::OnPlayerReleasedGhost(Player* player)
{
    FOREACH_SCRIPT(PlayerScript)->OnPlayerReleasedGhost(player);
}

void ScriptMgr::OnCompleteQuestChoice(Player* player, uint32 choiceId, uint32 responseId)
{
    FOREACH_SCRIPT(PlayerScript)->OnCompleteQuestChoice(player, choiceId, responseId);
}

void ScriptMgr::OnPlayerUnsummonPetTemporary(Player* player)
{
    FOREACH_SCRIPT(PlayerScript)->OnUnsummonPetTemporary(player);
}

 void ScriptMgr::OnPlayerResummonPetTemporary(Player* player)
{
    FOREACH_SCRIPT(PlayerScript)->OnResummonPetTemporary(player);
}

void ScriptMgr::OnPlayerMovementUpdate(Player* player)
{
    FOREACH_SCRIPT(PlayerScript)->OnMovementUpdate(player);
}

void ScriptMgr::OnPlayerStartChallengeMode(Player* player, uint8 level)
{
    FOREACH_SCRIPT(PlayerScript)->OnStartChallengeMode(player, level);
}

// Account
void ScriptMgr::OnAccountLogin(uint32 accountId)
{
    FOREACH_SCRIPT(AccountScript)->OnAccountLogin(accountId);
}

void ScriptMgr::OnPlayerItemLevelChange(Player* player)
{
    FOREACH_SCRIPT(PlayerScript)->OnItemLevelChange(player);
}

void ScriptMgr::OnFailedAccountLogin(uint32 accountId)
{
    FOREACH_SCRIPT(AccountScript)->OnFailedAccountLogin(accountId);
}

void ScriptMgr::OnEmailChange(uint32 accountId)
{
    FOREACH_SCRIPT(AccountScript)->OnEmailChange(accountId);
}

void ScriptMgr::OnFailedEmailChange(uint32 accountId)
{
    FOREACH_SCRIPT(AccountScript)->OnFailedEmailChange(accountId);
}

void ScriptMgr::OnPasswordChange(uint32 accountId)
{
    FOREACH_SCRIPT(AccountScript)->OnPasswordChange(accountId);
}

void ScriptMgr::OnFailedPasswordChange(uint32 accountId)
{
    FOREACH_SCRIPT(AccountScript)->OnFailedPasswordChange(accountId);
}

// Rest
void ScriptMgr::OnRestGetReceived(std::string url, RestResponse& response)
{
    FOR_SCRIPTS(RestScript, itr, end)
        if (itr->second->GetName() == url)
            itr->second->OnGet(response);
}

void ScriptMgr::OnRestPostReceived(std::string url, boost::property_tree::ptree tree, RestResponse& response)
{
    FOR_SCRIPTS(RestScript, itr, end)
        if (itr->second->GetName() == url)
            itr->second->OnPost(tree, response);
}

// Guild
void ScriptMgr::OnGuildAddMember(Guild* guild, Player* player, uint8& plRank)
{
    FOREACH_SCRIPT(GuildScript)->OnAddMember(guild, player, plRank);
}

void ScriptMgr::OnGuildRemoveMember(Guild* guild, ObjectGuid guid, bool isDisbanding, bool isKicked)
{
    FOREACH_SCRIPT(GuildScript)->OnRemoveMember(guild, guid, isDisbanding, isKicked);
}

void ScriptMgr::OnGuildMOTDChanged(Guild* guild, const std::string& newMotd)
{
    FOREACH_SCRIPT(GuildScript)->OnMOTDChanged(guild, newMotd);
}

void ScriptMgr::OnGuildInfoChanged(Guild* guild, const std::string& newInfo)
{
    FOREACH_SCRIPT(GuildScript)->OnInfoChanged(guild, newInfo);
}

void ScriptMgr::OnGuildCreate(Guild* guild, Player* leader, const std::string& name)
{
    FOREACH_SCRIPT(GuildScript)->OnCreate(guild, leader, name);
}

void ScriptMgr::OnGuildDisband(Guild* guild)
{
    FOREACH_SCRIPT(GuildScript)->OnDisband(guild);
}

void ScriptMgr::OnGuildMemberWitdrawMoney(Guild* guild, Player* player, uint64 &amount, bool isRepair)
{
    FOREACH_SCRIPT(GuildScript)->OnMemberWitdrawMoney(guild, player, amount, isRepair);
}

void ScriptMgr::OnGuildMemberDepositMoney(Guild* guild, Player* player, uint64 &amount)
{
    FOREACH_SCRIPT(GuildScript)->OnMemberDepositMoney(guild, player, amount);
}

void ScriptMgr::OnGuildItemMove(Guild* guild, Player* player, Item* pItem, bool isSrcBank, uint8 srcContainer, uint8 srcSlotId,
            bool isDestBank, uint8 destContainer, uint8 destSlotId)
{
    FOREACH_SCRIPT(GuildScript)->OnItemMove(guild, player, pItem, isSrcBank, srcContainer, srcSlotId, isDestBank, destContainer, destSlotId);
}

void ScriptMgr::OnGuildEvent(Guild* guild, uint8 eventType, ObjectGuid::LowType playerGuid1, ObjectGuid::LowType playerGuid2, uint8 newRank)
{
    FOREACH_SCRIPT(GuildScript)->OnEvent(guild, eventType, playerGuid1, playerGuid2, newRank);
}

void ScriptMgr::OnGuildBankEvent(Guild* guild, uint8 eventType, uint8 tabId, ObjectGuid::LowType playerGuid, uint64 itemOrMoney, uint16 itemStackCount, uint8 destTabId)
{
    FOREACH_SCRIPT(GuildScript)->OnBankEvent(guild, eventType, tabId, playerGuid, itemOrMoney, itemStackCount, destTabId);
}

void ScriptMgr::OnBoradcastToGuild(Guild const* me, WorldSession* session, bool officerOnly, std::string const& msg, uint32 language,Player* player, bool& Skip)
{
    FOREACH_SCRIPT(GuildScript)->OnBoradcastToGuild(me, session, officerOnly, msg, language, player, Skip);
}

void ScriptMgr::OnBroadcastPacketToRank(Guild const* me, WorldPacket const* packet, uint8 rankId, Player* player, bool& Skip)
{
    FOREACH_SCRIPT(GuildScript)->OnBroadcastPacketToRank(me, packet, rankId, player, Skip);
}

void ScriptMgr::OnBroadcastPacket(Guild const* me, WorldPacket const* packet, Player* player,bool& Skip)
{
    FOREACH_SCRIPT(GuildScript)->OnBroadcastPacket(me, packet, player, Skip);
}

void ScriptMgr::OnGuildGetMember(Guild* me, Player* player, bool& Skip)
{
    FOREACH_SCRIPT(GuildScript)->OnGuildGetMember(me, player, Skip);
}
// Group
void ScriptMgr::OnGroupAddMember(Group* group, ObjectGuid guid)
{
    ASSERT(group);
    FOREACH_SCRIPT(GroupScript)->OnAddMember(group, guid);
}

void ScriptMgr::OnGroupInviteMember(Group* group, ObjectGuid guid)
{
    ASSERT(group);
    FOREACH_SCRIPT(GroupScript)->OnInviteMember(group, guid);
}

void ScriptMgr::OnGroupRemoveMember(Group* group, ObjectGuid guid, RemoveMethod method, ObjectGuid kicker, const char* reason)
{
    ASSERT(group);
    FOREACH_SCRIPT(GroupScript)->OnRemoveMember(group, guid, method, kicker, reason);
}

void ScriptMgr::OnGroupChangeLeader(Group* group, ObjectGuid newLeaderGuid, ObjectGuid oldLeaderGuid)
{
    ASSERT(group);
    FOREACH_SCRIPT(GroupScript)->OnChangeLeader(group, newLeaderGuid, oldLeaderGuid);
}

void ScriptMgr::OnGroupDisband(Group* group)
{
    ASSERT(group);
    FOREACH_SCRIPT(GroupScript)->OnDisband(group);
}

void ScriptMgr::OnGroupBroadcastPacket(Group* me, WorldPacket const* packet, bool ignorePlayersInBGRaid, int group, ObjectGuid ignoredPlayer, bool& SkipOtherCode)
{
    FOREACH_SCRIPT(GroupScript)->OnGroupBroadcastPacket( me,  packet, ignorePlayersInBGRaid, group, ignoredPlayer,  SkipOtherCode);
}
// Unit
void ScriptMgr::OnHeal(Unit* healer, Unit* reciever, uint32& gain)
{
    FOREACH_SCRIPT(UnitScript)->OnHeal(healer, reciever, gain);
    FOREACH_SCRIPT(PlayerScript)->OnHeal(healer, reciever, gain);
}

void ScriptMgr::OnDamage(Unit* attacker, Unit* victim, uint32& damage, SpellInfo const* spellProto)
{
    FOREACH_SCRIPT(UnitScript)->OnDamage(attacker, victim, damage, spellProto);
    FOREACH_SCRIPT(PlayerScript)->OnDamage(attacker, victim, damage, spellProto);
}

void ScriptMgr::ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage)
{
    FOREACH_SCRIPT(UnitScript)->ModifyPeriodicDamageAurasTick(target, attacker, damage);
    FOREACH_SCRIPT(PlayerScript)->ModifyPeriodicDamageAurasTick(target, attacker, damage);
}

void ScriptMgr::ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage)
{
    FOREACH_SCRIPT(UnitScript)->ModifyMeleeDamage(target, attacker, damage);
    FOREACH_SCRIPT(PlayerScript)->ModifyMeleeDamage(target, attacker, damage);
}

void ScriptMgr::ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo)
{
    FOREACH_SCRIPT(UnitScript)->ModifySpellDamageTaken(target, attacker, damage, spellInfo);
    FOREACH_SCRIPT(PlayerScript)->ModifySpellDamageTaken(target, attacker, damage, spellInfo);
}

// Conversation
void ScriptMgr::OnConversationCreate(Conversation* conversation, Unit* creator)
{
    ASSERT(conversation);

    GET_SCRIPT(ConversationScript, conversation->GetScriptId(), tmpscript);
    tmpscript->OnConversationCreate(conversation, creator);
}

// Scene
void ScriptMgr::OnSceneStart(Player* player, uint32 sceneInstanceID, SceneTemplate const* sceneTemplate)
{
    ASSERT(player);
    ASSERT(sceneTemplate);

    GET_SCRIPT(SceneScript, sceneTemplate->ScriptId, tmpscript);
    tmpscript->OnSceneStart(player, sceneInstanceID, sceneTemplate);
}

void ScriptMgr::OnSceneTrigger(Player* player, uint32 sceneInstanceID, SceneTemplate const* sceneTemplate, std::string const& triggerName)
{
    ASSERT(player);
    ASSERT(sceneTemplate);

    GET_SCRIPT(SceneScript, sceneTemplate->ScriptId, tmpscript);
    tmpscript->OnSceneTriggerEvent(player, sceneInstanceID, sceneTemplate, triggerName);
}

void ScriptMgr::OnSceneCancel(Player* player, uint32 sceneInstanceID, SceneTemplate const* sceneTemplate)
{
    ASSERT(player);
    ASSERT(sceneTemplate);

    GET_SCRIPT(SceneScript, sceneTemplate->ScriptId, tmpscript);
    tmpscript->OnSceneCancel(player, sceneInstanceID, sceneTemplate);
    tmpscript->OnSceneEnd(player, sceneInstanceID, sceneTemplate);
}

void ScriptMgr::OnSceneComplete(Player* player, uint32 sceneInstanceID, SceneTemplate const* sceneTemplate)
{
    ASSERT(player);
    ASSERT(sceneTemplate);

    GET_SCRIPT(SceneScript, sceneTemplate->ScriptId, tmpscript);
    tmpscript->OnSceneComplete(player, sceneInstanceID, sceneTemplate);
    tmpscript->OnSceneEnd(player, sceneInstanceID, sceneTemplate);
}

void ScriptMgr::OnQuestStatusChange(Player* player, Quest const* quest, QuestStatus oldStatus, QuestStatus newStatus)
{
    ASSERT(player);
    ASSERT(quest);

    GET_SCRIPT(QuestScript, quest->GetScriptId(), tmpscript);
    tmpscript->OnQuestStatusChange(player, quest, oldStatus, newStatus);
}

void ScriptMgr::OnQuestObjectiveChange(Player* player, Quest const* quest, QuestObjective const& objective, int32 oldAmount, int32 newAmount)
{
    ASSERT(player);
    ASSERT(quest);

    GET_SCRIPT(QuestScript, quest->GetScriptId(), tmpscript);
    tmpscript->OnQuestObjectiveChange(player, quest, objective, oldAmount, newAmount);
}

void ScriptMgr::OnPlayerMove(Player* player, MovementInfo movementInfo, uint32 opcode)
{
    FOREACH_SCRIPT(MovementHandlerScript)->OnPlayerMove(player, movementInfo, opcode);
}

void ScriptMgr::OnSendToAll(Channel const* me, Player* player, ObjectGuid const& guid, bool& Skip)
{
    FOREACH_SCRIPT(ChannelScript)->OnSendToAll(me, player, guid, Skip);
}

void ScriptMgr::OnSendToAllButOne(Channel const* me, Player* player, ObjectGuid const& guid, bool& Skip)
{
    FOREACH_SCRIPT(ChannelScript)->OnSendToAllButOne(me, player, guid, Skip);
}

void ScriptMgr::OnSendToOne(Channel const* me, Player* player, ObjectGuid const& guid, bool& Skip)
{
    FOREACH_SCRIPT(ChannelScript)->OnSendToOne(me, player, guid, Skip);
}

void ScriptMgr::OnSendToAllWatching(Channel* me, Player* player, ObjectGuid const& guid, bool& Skip)
{
    FOREACH_SCRIPT(ChannelScript)->OnSendToAllWatching(me, player, guid, Skip);
}

void ScriptMgr::OnHandleJoinChannel(Channel* me, Player* player, ObjectGuid const& guid, bool& Skip)
{
    FOREACH_SCRIPT(ChannelScript)->OnHandleJoinChannel(me, player, guid, Skip);
}

void ScriptMgr::OnListChannel(Channel* me,Player const* player,bool& Skip)
{
    FOREACH_SCRIPT(ChannelScript)->OnListChannel(me, player, Skip);
}
void ScriptMgr::OnHandleContactListOpcode(bool& SkipCoreCode, WorldSession* me, WorldPacket& recv_data, Player* player)
{
    FOREACH_SCRIPT(SocialScript)->OnHandleContactListOpcode(SkipCoreCode, me, recv_data, player);
}

void ScriptMgr::OnHandleWhoOpcode( WorldSession* me, WorldPackets::Who::WhoRequestPkt& whoRequest, Player* player, bool& Skip)
{
    FOREACH_SCRIPT(SocialScript)->OnHandleWhoOpcode( me, whoRequest,  player, Skip);
}

void ScriptMgr::OnBroadcastToFriendListers(bool& SkipCoreCode, SocialMgr* me, Player* player, WorldPacket* packet, SocialMgr::SocialMap& m_socialMap)
{
    FOREACH_SCRIPT(SocialScript)->OnBroadcastToFriendListers(SkipCoreCode, me, player, packet, m_socialMap);
}

void ScriptMgr::OnSendSocialList( PlayerSocial* me, Player* player, uint32 flags, bool& Skip)
{
    FOREACH_SCRIPT(SocialScript)->OnSendSocialList(me, player, flags, Skip);
}
void ScriptMgr::OnGetFriendInfo(SocialMgr* me, Player* player, ObjectGuid const& friendGUID, FriendInfo& friendInfo, PlayerSocial::PlayerSocialMap _playerSocialMap,bool& Skip)
{
    FOREACH_SCRIPT(SocialScript)->OnGetFriendInfo(me, player, friendGUID, friendInfo, _playerSocialMap, Skip);
}
void ScriptMgr::OnSendFriendStatus(SocialMgr* me, Player* player, FriendsResult result, ObjectGuid const& friendGuid, bool broadcast ,bool& Skip)
{
    FOREACH_SCRIPT(SocialScript)->OnSendFriendStatus(me, player, result, friendGuid, broadcast, Skip);
}
void ScriptMgr::OnBroadcastToFriendListers(SocialMgr* me,Player* player, WorldPacket const* packet, SocialMgr::SocialMap* _socialMap, bool& Skip)
{
    FOREACH_SCRIPT(SocialScript)->OnBroadcastToFriendListers(me, player, packet, _socialMap, Skip);
}
void ScriptMgr::OnIsVisibleGloballyFor(Player const* me,Player const* u,bool& Skip)
{
    FOREACH_SCRIPT(SocialScript)->OnIsVisibleGloballyFor(me, u, Skip);
}
SpellScriptLoader::SpellScriptLoader(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<SpellScriptLoader>::Instance()->AddScript(this);
}

ServerScript::ServerScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<ServerScript>::Instance()->AddScript(this);
}

WorldScript::WorldScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<WorldScript>::Instance()->AddScript(this);
}

FormulaScript::FormulaScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<FormulaScript>::Instance()->AddScript(this);
}

UnitScript::UnitScript(const char* name, bool addToScripts)
    : ScriptObject(name)
{
    if (addToScripts)
        ScriptRegistry<UnitScript>::Instance()->AddScript(this);
}

WorldMapScript::WorldMapScript(const char* name, uint32 mapId)
    : ScriptObject(name), MapScript<Map>(sMapStore.LookupEntry(mapId))
{
    if (!GetEntry())
        TC_LOG_ERROR("scripts", "Invalid WorldMapScript for %u; no such map ID.", mapId);

    if (GetEntry() && !GetEntry()->IsWorldMap())
        TC_LOG_ERROR("scripts", "WorldMapScript for map %u is invalid.", mapId);

    ScriptRegistry<WorldMapScript>::Instance()->AddScript(this);
}

InstanceMapScript::InstanceMapScript(const char* name, uint32 mapId)
    : ScriptObject(name), MapScript<InstanceMap>(sMapStore.LookupEntry(mapId))
{
    if (!GetEntry())
        TC_LOG_ERROR("scripts", "Invalid InstanceMapScript for %u; no such map ID.", mapId);

    if (GetEntry() && !GetEntry()->IsDungeon())
        TC_LOG_ERROR("scripts", "InstanceMapScript for map %u is invalid.", mapId);

    ScriptRegistry<InstanceMapScript>::Instance()->AddScript(this);
}

BattlegroundMapScript::BattlegroundMapScript(const char* name, uint32 mapId)
    : ScriptObject(name), MapScript<BattlegroundMap>(sMapStore.LookupEntry(mapId))
{
    if (!GetEntry())
        TC_LOG_ERROR("scripts", "Invalid BattlegroundMapScript for %u; no such map ID.", mapId);

    if (GetEntry() && !GetEntry()->IsBattleground())
        TC_LOG_ERROR("scripts", "BattlegroundMapScript for map %u is invalid.", mapId);

    ScriptRegistry<BattlegroundMapScript>::Instance()->AddScript(this);
}

ItemScript::ItemScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<ItemScript>::Instance()->AddScript(this);
}

CreatureScript::CreatureScript(const char* name)
    : UnitScript(name, false)
{
    ScriptRegistry<CreatureScript>::Instance()->AddScript(this);
}

uint32 CreatureScript::GetDialogStatus(Player* /*player*/, Creature* /*creature*/)
{
    return DIALOG_STATUS_SCRIPTED_NO_STATUS;
}

GameObjectScript::GameObjectScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<GameObjectScript>::Instance()->AddScript(this);
}

uint32 GameObjectScript::GetDialogStatus(Player* /*player*/, GameObject* /*go*/)
{
    return DIALOG_STATUS_SCRIPTED_NO_STATUS;
}

AreaTriggerScript::AreaTriggerScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<AreaTriggerScript>::Instance()->AddScript(this);
}

BattlegroundScript::BattlegroundScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<BattlegroundScript>::Instance()->AddScript(this);
}

OutdoorPvPScript::OutdoorPvPScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<OutdoorPvPScript>::Instance()->AddScript(this);
}

CommandScript::CommandScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<CommandScript>::Instance()->AddScript(this);
}

WeatherScript::WeatherScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<WeatherScript>::Instance()->AddScript(this);
}

AuctionHouseScript::AuctionHouseScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<AuctionHouseScript>::Instance()->AddScript(this);
}

ConditionScript::ConditionScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<ConditionScript>::Instance()->AddScript(this);
}

VehicleScript::VehicleScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<VehicleScript>::Instance()->AddScript(this);
}

DynamicObjectScript::DynamicObjectScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<DynamicObjectScript>::Instance()->AddScript(this);
}

TransportScript::TransportScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<TransportScript>::Instance()->AddScript(this);
}

AchievementCriteriaScript::AchievementCriteriaScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<AchievementCriteriaScript>::Instance()->AddScript(this);
}

PlayerScript::PlayerScript(const char* name)
    : UnitScript(name, false)
{
    ScriptRegistry<PlayerScript>::Instance()->AddScript(this);
}

AccountScript::AccountScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<AccountScript>::Instance()->AddScript(this);
}

RestScript::RestScript(const char* url)
    : ScriptObject(url)
{
    ScriptRegistry<RestScript>::Instance()->AddScript(this);
}

SceneScript::SceneScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<SceneScript>::Instance()->AddScript(this);
}

QuestScript::QuestScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<QuestScript>::Instance()->AddScript(this);
}

GuildScript::GuildScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<GuildScript>::Instance()->AddScript(this);
}

GroupScript::GroupScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<GroupScript>::Instance()->AddScript(this);
}

AreaTriggerEntityScript::AreaTriggerEntityScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<AreaTriggerEntityScript>::Instance()->AddScript(this);
}

GarrisonScript::GarrisonScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<GarrisonScript>::Instance()->AddScript(this);
}

ConversationScript::ConversationScript(char const* name)
    : ScriptObject(name)
{
    ScriptRegistry<ConversationScript>::Instance()->AddScript(this);
}

ZoneScript::ZoneScript(const char* name)
    : ScriptObject(name), _scriptType(ZONE_SCRIPT_TYPE_ZONE)
{
    ScriptRegistry<ZoneScript>::Instance()->AddScript(this);
}

MovementHandlerScript::MovementHandlerScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<MovementHandlerScript>::Instance()->AddScript(this);
}

AllCreatureScript::AllCreatureScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<AllCreatureScript>::Instance()->AddScript(this);
}

ChannelScript::ChannelScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<ChannelScript>::Instance()->AddScript(this);
}
SocialScript::SocialScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<SocialScript>::Instance()->AddScript(this);
}

// Specialize for each script type class like so:
template class TC_GAME_API ScriptRegistry<SpellScriptLoader>;
template class TC_GAME_API ScriptRegistry<ServerScript>;
template class TC_GAME_API ScriptRegistry<WorldScript>;
template class TC_GAME_API ScriptRegistry<FormulaScript>;
template class TC_GAME_API ScriptRegistry<WorldMapScript>;
template class TC_GAME_API ScriptRegistry<InstanceMapScript>;
template class TC_GAME_API ScriptRegistry<BattlegroundMapScript>;
template class TC_GAME_API ScriptRegistry<ItemScript>;
template class TC_GAME_API ScriptRegistry<CreatureScript>;
template class TC_GAME_API ScriptRegistry<GameObjectScript>;
template class TC_GAME_API ScriptRegistry<AreaTriggerScript>;
template class TC_GAME_API ScriptRegistry<BattlegroundScript>;
template class TC_GAME_API ScriptRegistry<OutdoorPvPScript>;
template class TC_GAME_API ScriptRegistry<CommandScript>;
template class TC_GAME_API ScriptRegistry<WeatherScript>;
template class TC_GAME_API ScriptRegistry<AuctionHouseScript>;
template class TC_GAME_API ScriptRegistry<ConditionScript>;
template class TC_GAME_API ScriptRegistry<VehicleScript>;
template class TC_GAME_API ScriptRegistry<DynamicObjectScript>;
template class TC_GAME_API ScriptRegistry<TransportScript>;
template class TC_GAME_API ScriptRegistry<AchievementCriteriaScript>;
template class TC_GAME_API ScriptRegistry<PlayerScript>;
template class TC_GAME_API ScriptRegistry<GuildScript>;
template class TC_GAME_API ScriptRegistry<GroupScript>;
template class TC_GAME_API ScriptRegistry<UnitScript>;
template class TC_GAME_API ScriptRegistry<AccountScript>;
template class TC_GAME_API ScriptRegistry<RestScript>;
template class TC_GAME_API ScriptRegistry<AreaTriggerEntityScript>;
template class TC_GAME_API ScriptRegistry<GarrisonScript>;
template class TC_GAME_API ScriptRegistry<ConversationScript>;
template class TC_GAME_API ScriptRegistry<SceneScript>;
template class TC_GAME_API ScriptRegistry<QuestScript>;
template class TC_GAME_API ScriptRegistry<ZoneScript>;
template class TC_GAME_API ScriptRegistry<AllCreatureScript>;
template class TC_GAME_API ScriptRegistry<MovementHandlerScript>;
template class TC_GAME_API ScriptRegistry<ChannelScript>;
template class TC_GAME_API ScriptRegistry<SocialScript>;
