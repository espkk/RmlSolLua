#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <RmlSolLua_private.h>
#include <RmlUi/Core.h>
#include <RmlUi/Core/Debug.h>
#include SOLHPP
#include <sol/table_proxy.hpp>

#include "SolLuaDataModel.h"

#include "SolLuaDocument.h"
#include "bind/bind.h"

namespace Rml::SolLua
{
	namespace
	{
		// Proxy definition to return self as a scalar definition (for non-table keys)
		class ScalarDefinitionProxy final : public VariableDefinition
		{
		public:
			ScalarDefinitionProxy(SolLuaDataModelProxy* self)
			    : VariableDefinition(DataVariableType::Scalar), m_self(self)
			{
			}

			bool Get(void* ptr, Variant& variant) override
			{
				return m_self->Get(ptr, variant);
			}

			bool Set(void* ptr, const Variant& variant) override
			{
				return m_self->Set(ptr, variant);
			}

			SolLuaDataModelProxy* m_self;
		};

		class LiteralIntDefinition final : public VariableDefinition
		{
		public:
			LiteralIntDefinition()
			    : VariableDefinition(DataVariableType::Scalar)
			{
			}

			bool Get(void* ptr, Variant& variant) override
			{
				variant = static_cast<int>(reinterpret_cast<intptr_t>(ptr));
				return true;
			}
		} sizeDef;

		bool IsArrayIndex(std::string_view key)
		{
			return key.starts_with('[');
		}

		// @pre Key is a valid null-terminated string
		bool IsArrayIndex(const char* key)
		{
			return key[0] == '[';
		}

		// Registry of Lua-created data models, keyed by (Context*, name).
		// Owns one strong shared_ptr per active model; OnDataModelDestroy releases it.
		// Lua userdata holds another aliased shared_ptr, so the C++ memory survives
		// until the last Lua ref is collected — at which point the disposed flag
		// (set during NotifyDestroyed) has already made all operations inert.
		using DataModelRegistry = std::unordered_map<Rml::Context*, std::unordered_map<Rml::String, std::shared_ptr<SolLuaDataModel>>>;

		DataModelRegistry& GetDataModelRegistry()
		{
			static DataModelRegistry registry;
			return registry;
		}

	} // namespace

	SolLuaDataModel::SolLuaDataModel(const sol::table& model, const Rml::DataModelConstructor& constructor)
	    : m_constructor(constructor),
	      m_topLevelProxy(this, model)
	{
		m_topLevelProxy.bind();
	}

	std::shared_ptr<SolLuaDataModel> SolLuaDataModel::GetOrCreate(
	    Rml::Context* context, const Rml::String& name, const sol::table& model, const Rml::DataModelConstructor& constructor
	)
	{
		auto& context_models = GetDataModelRegistry()[context];
		auto it = context_models.find(name);
		if (it == context_models.end())
		{
			auto ptr = std::make_shared<SolLuaDataModel>(model, constructor);
			context_models.emplace(name, ptr);
			return ptr;
		}

		// Re-opening: keep the same C++ model (RmlUi bindings are immutable) and
		// rebind the root table so subsequent reads/writes see the new data.
		it->second->m_constructor = constructor;
		it->second->m_topLevelProxy.rebindRoot(model);
		return it->second;
	}

	void SolLuaDataModel::NotifyDestroyed(Rml::Context* context, const Rml::String& name)
	{
		auto& registry = GetDataModelRegistry();
		auto context_it = registry.find(context);
		if (context_it == registry.end())
		{
			return;
		}

		auto model_it = context_it->second.find(name);
		if (model_it != context_it->second.end())
		{
			auto& model = *model_it->second;
			model.m_disposed = true;
			model.m_constructor = {};
			model.m_topLevelProxy.markDisposed();
			context_it->second.erase(model_it);
		}

		if (context_it->second.empty())
		{
			registry.erase(context_it);
		}
	}

	/// SolLuaDatamodelProxy
	SolLuaDataModelProxy::SolLuaDataModelProxy(SolLuaDataModel* datamodel, sol::table table)
	    : VariableDefinition(DataVariableType::Struct),
	      m_datamodel(datamodel), m_table(std::move(table)), m_selfAsScalar(std::make_unique<ScalarDefinitionProxy>(this))
	{
	}

	bool SolLuaDataModelProxy::Get(void* ptr, Rml::Variant& variant)
	{
		if (ptr == nullptr)
		{
			// Allow RmlUi's expression engine to pass a proxy to an event handler.
			// luaUserdata() will be called lazily during variant conversion.
			variant = this;
			return true;
		}

		sol::object obj;
		auto* key = const_cast<const char*>(static_cast<char*>(ptr));
		if (IsArrayIndex(key))
		{
			// Pseudo-key: access by index
			int idx;
			std::from_chars_result result = std::from_chars(key + 1, key + std::strlen(key) - 1, idx);
			RMLUI_ASSERT(result.ec == std::errc{} && "Rml failed to sanitize user input to be well-formed");
			if (idx < 0 || idx >= m_table.size())
			{
				Rml::Log::Message(Rml::Log::LT_ERROR, "[LUA][ERROR] Data array index out of bounds");
				RMLUI_ERROR;
				return false;
			}
			obj = m_table[idx + 1]; // Lua is 1-based
		}
		else
		{
			obj = m_table[key];
		}

		if (obj.is<bool>())
		{
			variant = obj.as<bool>();
		}
		else if (obj.is<std::string>())
		{
			variant = obj.as<std::string>();
		}
		else if (obj.is<Rml::Vector2i>())
		{
			variant = obj.as<Vector2i>();
		}
		else if (obj.is<Rml::Vector2f>())
		{
			variant = obj.as<Vector2f>();
		}
		else if (obj.is<Rml::Colourb>())
		{
			variant = obj.as<Rml::Colourb>();
		}
		else if (obj.is<Rml::Colourf>())
		{
			variant = obj.as<Rml::Colourf>();
		}
		else if (obj.is<double>())
		{
			variant = obj.as<double>();
		}

		return true;
	}

	bool SolLuaDataModelProxy::Set(void* ptr, const Rml::Variant& variant)
	{
		RMLUI_ASSERT(variant.GetType() != Variant::VOIDPTR && "VOIDPTR is reserved for datamodel reference and unexpected in this context."
		                                                      "If this assert breaks, we need to change the approach (see `Get()`).");

		if (ptr == nullptr)
		{
			Rml::Log::Message(Rml::Log::LT_ERROR, "[LUA][ERROR] Trying to access a table as a scalar from VariableDefinition::Set");
			RMLUI_ERROR;
			return false;
		}

		auto* key = const_cast<const char*>(static_cast<char*>(ptr));
		if (IsArrayIndex(key))
		{
			int idx;
			std::from_chars_result result = std::from_chars(key + 1, key + std::strlen(key) - 1, idx);
			RMLUI_ASSERT(result.ec == std::errc{} && "RmlUi failed to sanitize user input to be well-formed");
			if (idx < 0 || idx >= m_table.size())
			{
				Rml::Log::Message(Rml::Log::LT_ERROR, "[LUA][ERROR] Data array index out of bounds");
				RMLUI_ERROR;
				return false;
			}
			m_table[idx + 1] = makeObjectFromVariant(&variant, m_table.lua_state());
		}
		else
		{
			m_table[key] = makeObjectFromVariant(&variant, m_table.lua_state());
		}
		return true;
	}

	int SolLuaDataModelProxy::Size(void* ptr)
	{
		if (ptr != nullptr)
		{
			Rml::Log::Message(Rml::Log::LT_ERROR, "[LUA][ERROR] Trying to get size of a scalar");
			RMLUI_ERROR;
			return 0;
		}
		return static_cast<int>(m_table.size());
	}

	DataVariable SolLuaDataModelProxy::Child(void* ptr, const Rml::DataAddressEntry& address)
	{
		if (ptr != nullptr)
		{
			Rml::Log::Message(Rml::Log::LT_ERROR, "[LUA][ERROR] Trying to access a sub element of a scalar");
			RMLUI_ERROR;
			return {};
		}

		std::string skey;
		sol::object obj;
		if (address.index != -1)
		{
			// Table treated as array (e.g. data-for and co)
			if (address.index < 0 || address.index >= m_table.size())
			{
				Rml::Log::Message(Rml::Log::LT_ERROR, "[LUA][ERROR] Data array index out of bounds");
				RMLUI_ERROR;
				return {};
			}
			// Access by index
			skey = std::format("[{}]", address.index);
			obj = m_table[address.index + 1]; // Lua is 1-based
		}
		else if (IsArrayIndex(address.name))
		{
			// Table treated as struct (e.g. via reflection)
			RMLUI_ASSERT(address.name.ends_with(']'));
			RMLUI_ASSERT(address.name.size() > 2);

			const std::string_view indexStr(address.name.data() + 1, address.name.size() - 2);
			std::int32_t index = -1;
			[[maybe_unused]] auto [end, ec] = std::from_chars(indexStr.data(), indexStr.data() + indexStr.size(), index);
			RMLUI_ASSERT(ec == std::errc{});
			RMLUI_ASSERT(end == indexStr.data() + indexStr.size());

			skey = address.name;
			obj = m_table[index + 1]; // Lua is 1-based
		}
		else
		{
			if (address.name == "size")
			{
				return {&sizeDef, reinterpret_cast<void*>(static_cast<intptr_t>(m_table.size()))};
			}

			skey = address.name;
			obj = m_table[address.name];
		}

		if (obj.get_type() == sol::type::lua_nil)
		{
			return {};
		}

		if (obj.get_type() == sol::type::table)
		{
			auto [it, inserted] = m_children.try_emplace(skey, m_datamodel, obj.as<sol::table>());
			if (inserted)
			{
				it->second.m_topLevelKey = isTopLevel() ? &it->first : m_topLevelKey;
			}
			return {&it->second, nullptr};
		}

		// Create scalar key entry on-demand
		auto [it, _] = m_keys.emplace(skey);
		return {m_selfAsScalar.get(), const_cast<char*>(it->data())};
	}

	StringList SolLuaDataModelProxy::ReflectMemberNames()
	{
		StringList names;
		for (const auto& key : m_table | std::views::keys)
		{
			if (key.get_type() == sol::type::string)
			{
				names.push_back(key.as<std::string>());
			}
			else if (key.get_type() == sol::type::number)
			{
				const double number = key.as<double>() - 1;
				const bool isInteger = std::isfinite(number) && std::floor(number) == number;
				if (isInteger && number >= 0)
				{
					names.push_back(std::format("[{}]", static_cast<std::uint64_t>(number)));
				}
				else
				{
					Rml::Log::Message(Rml::Log::LT_WARNING, "[LUA][WARNING] Data model key '%g' is not a valid non-negative integer index, skipping", key.as<double>());
				}
			}
		}
		return names;
	}

	sol::object& SolLuaDataModelProxy::luaUserdata()
	{
		// Never (re)create the cached userdata for a disposed proxy: doing so would
		// re-establish the self-referential cycle that markDisposed() just broke.
		if (!m_luaUserdata.valid() && !isDisposed())
		{
			cacheUserdata();
		}
		return m_luaUserdata;
	}

	void SolLuaDataModelProxy::attachRawTableAsUservalueTo(sol::object& target) const
	{
		lua_State* L = m_table.lua_state();
		target.push(L);          // [ud]
		m_table.push(L);         // [ud, tbl]
		lua_setuservalue(L, -2); // [ud]
		lua_pop(L, 1);           // []
	}

	void SolLuaDataModelProxy::cacheUserdata()
	{
		lua_State* L = m_table.lua_state();
		// Aliased shared_ptr: shares ownership with the model so the C++ memory
		// outlives native disposal as long as Lua holds any reference.
		std::shared_ptr<SolLuaDataModelProxy> aliased(m_datamodel->shared_from_this(), this);
		m_luaUserdata = sol::make_object(L, std::move(aliased));
		attachRawTableAsUservalueTo(m_luaUserdata);
	}

	bool SolLuaDataModelProxy::isTopLevel() const
	{
		return m_topLevelKey == nullptr;
	}

	bool SolLuaDataModelProxy::isDisposed() const
	{
		return m_datamodel == nullptr || m_datamodel->isDisposed();
	}

	void SolLuaDataModelProxy::registerTopLevelTable(const std::string& key, const sol::table& table)
	{
		auto [it, inserted] = m_children.try_emplace(key, m_datamodel, table);
		if (inserted)
		{
			it->second.m_topLevelKey = &it->first;
			m_datamodel->constructor().BindCustomDataVariable(
			    key, Rml::DataVariable(&it->second, nullptr)
			);
			return;
		}

		it->second.rebind(table);
	}

	void SolLuaDataModelProxy::registerTopLevelScalar(const std::string& key)
	{
		auto [it, inserted] = m_keys.emplace(key);
		if (inserted)
		{
			m_datamodel->constructor().BindCustomDataVariable(
			    key, Rml::DataVariable(m_selfAsScalar.get(), const_cast<char*>(it->data()))
			);
		}
	}

	void SolLuaDataModelProxy::registerTopLevelCallback(const std::string& key, const sol::protected_function& /*func*/, lua_State* L)
	{
		// RmlUi's BindEventCallback uses emplace - the first lambda we bind for
		// a given key stays installed forever. So bind once with an indirection
		// lambda that looks up the current function in m_table at call time;
		// hot-reload (rebindRoot) replaces m_table, so the lookup naturally
		// resolves to the new closure without any C++-side mirror of the function.
		if (!m_datamodel->boundCallbacks().insert(key).second)
		{
			return;
		}

		m_datamodel->constructor().BindEventCallback(
		    key,
		    [proxy = this, key, state = sol::state_view{L}](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList& varlist)
		    {
			    if (proxy->m_datamodel->isDisposed())
			    {
				    return;
			    }
			    sol::object obj = proxy->m_table[key];
			    if (obj.get_type() != sol::type::function)
			    {
				    return;
			    }
			    sol::protected_function cb(obj);
			    std::vector<sol::object> args;
			    for (const auto& variant : varlist)
			    {
				    args.push_back(makeObjectFromVariant(&variant, state));
			    }
			    auto pfr = cb(event, sol::as_args(args));
			    if (!pfr.valid())
			    {
				    ErrorHandler(cb.lua_state(), std::move(pfr));
			    }
		    }
		);
	}

	sol::object SolLuaDataModelProxy::luaIndex(SolLuaDataModelProxy& self, sol::stack_object key, sol::this_state ts)
	{
		std::string skey = key.is<std::string>() ? key.as<std::string>() : std::format("[{}]", key.as<int>() - 1);

		auto it = self.m_children.find(skey);
		if (it != self.m_children.end())
		{
			return it->second.luaUserdata();
		}

		// Lookup in the uservalue table (the backing Lua table).
		// If the model has been disposed, m_table (and the uservalue) is empty,
		// so this returns nil naturally without a special-case branch.
		lua_State* L = ts;
		lua_getuservalue(L, 1); // [tbl]
		key.push(L);            // [tbl, key]
		lua_gettable(L, -2);    // [tbl, value]
		sol::object result(L, -1);
		lua_pop(L, 2); // []
		return result;
	}

	void SolLuaDataModelProxy::luaNewIndex(SolLuaDataModelProxy& self, sol::stack_object key, sol::stack_object value, sol::this_state ts)
	{
		// After native disposal m_constructor is cleared; calling DirtyVariable on
		// it would null-deref the underlying DataModel*. Short-circuit instead.
		if (self.isDisposed())
		{
			return;
		}

		std::string skey = key.is<std::string>() ? key.as<std::string>() : std::format("[{}]", key.as<int>() - 1);
		const bool isTopLevelNamed = self.isTopLevel() && !IsArrayIndex(skey);

		// Top-level type changes (scalar<->table) are forbidden because RmlUi's
		// BindVariable uses emplace - bindings cannot be replaced after creation.
		if (isTopLevelNamed)
		{
			if (value.get_type() == sol::type::table && self.m_keys.contains(skey))
			{
				Rml::Log::Message(Rml::Log::LT_ERROR, "[LUA][ERROR] Cannot change top-level scalar '%s' to a table: "
				                                      "RmlUi bindings are immutable after creation",
				                  skey.c_str());
				return;
			}
			if (value.get_type() != sol::type::table && value.get_type() != sol::type::lua_nil && self.m_children.contains(skey))
			{
				Rml::Log::Message(Rml::Log::LT_ERROR, "[LUA][ERROR] Cannot change top-level table '%s' to a non-table: "
				                                      "RmlUi bindings are immutable after creation",
				                  skey.c_str());
				return;
			}
		}

		// Store into the uservalue table (the backing Lua table).
		{
			lua_State* L = ts;
			lua_getuservalue(L, 1); // [tbl]
			key.push(L);            // [tbl, key]
			value.push(L);          // [tbl, key, value]
			lua_settable(L, -3);    // [tbl]
			lua_pop(L, 1);          // []
		}

		if (value.get_type() == sol::type::table)
		{
			auto proxyIt = self.m_children.find(skey);
			if (proxyIt != self.m_children.end())
			{
				proxyIt->second.rebind(value.as<sol::table>());
			}
			else if (isTopLevelNamed)
			{
				self.registerTopLevelTable(skey, value.as<sol::table>());
			}

			if (!self.isTopLevel())
			{
				self.m_keys.erase(skey);
			}
		}
		else if (value.get_type() == sol::type::lua_nil)
		{
			if (self.isTopLevel())
			{
				// Top-level removal: keep proxies/keys alive (RmlUi holds raw pointers).
				// Rebind any child proxy to an empty table so descendants return empty values.
				auto proxyIt = self.m_children.find(skey);
				if (proxyIt != self.m_children.end())
				{
					proxyIt->second.rebind(sol::table(ts, sol::create));
				}
			}
			else
			{
				self.m_children.erase(skey);
				self.m_keys.erase(skey);
			}
		}
		else if (value.get_type() == sol::type::function && isTopLevelNamed)
		{
			self.registerTopLevelCallback(skey, sol::protected_function{value}, ts);
			return;
		}
		else
		{
			if (!self.isTopLevel())
			{
				self.m_children.erase(skey);
			}

			if (isTopLevelNamed && !self.m_keys.contains(skey))
			{
				self.registerTopLevelScalar(skey);
			}
		}

		self.m_datamodel->constructor().GetModelHandle().DirtyVariable(self.isTopLevel() ? skey : *self.m_topLevelKey);
	}

	void SolLuaDataModelProxy::bind()
	{
		for (auto& [key, value] : m_table)
		{
			std::string skey;
			if (key.get_type() == sol::type::string)
			{
				skey = key.as<std::string>();
			}
			else if (key.get_type() == sol::type::number)
			{
				const double number = key.as<double>() - 1;
				const bool isInteger = std::isfinite(number) && std::floor(number) == number;
				if (isInteger && number >= 0)
				{
					skey = std::format("[{}]", static_cast<std::uint64_t>(number));
				}
			}
			if (skey.empty())
			{
				Rml::Log::Message(Log::LT_ERROR, "[LUA][ERROR] Data model key other than non-empty string or non-negative integer is unsupported");
				return;
			}

			if (IsArrayIndex(skey))
			{
				continue;
			}

			if (value.get_type() == sol::type::table)
			{
				registerTopLevelTable(skey, value.as<sol::table>());
			}
			else if (value.get_type() == sol::type::function)
			{
				registerTopLevelCallback(skey, sol::protected_function{value}, m_table.lua_state());
			}
			else
			{
				registerTopLevelScalar(skey);
			}
		}
	}

	void SolLuaDataModelProxy::rebindRoot(const sol::table& newTable)
	{
		RMLUI_ASSERT(isTopLevel() && "Root rebind should only be done on the top-level table");

		m_table = newTable;
		if (m_luaUserdata.valid())
		{
			attachRawTableAsUservalueTo(m_luaUserdata);
		}

		// Children that disappeared (or whose value is no longer a table) need to
		// be rebound to empty - RmlUi still holds raw pointers to their proxies.
		// Children still present as tables are rebound by registerTopLevelTable
		// when bind() walks the new table below. Top-level callbacks need no
		// special handling: the indirection lambda dispatches via m_table at
		// call time, and m_table has just been replaced above.
		for (auto& [key, child] : m_children)
		{
			sol::object obj = m_table[key];
			if (obj.get_type() != sol::type::table)
			{
				child.rebind(sol::table(m_table.lua_state(), sol::create));
			}
		}

		// Register any keys new to this table; idempotent for existing ones
		// (registerTopLevelScalar/Table/Callback all no-op or rebind in place).
		bind();

		auto handle = m_datamodel->constructor().GetModelHandle();
		for (const auto& key : m_keys)
		{
			handle.DirtyVariable(key);
		}
		for (const auto& [key, _] : m_children)
		{
			handle.DirtyVariable(key);
		}
	}

	void SolLuaDataModelProxy::rebind(const sol::table& newTable)
	{
		RMLUI_ASSERT(!isTopLevel() && "Rebind should never be done on top-level table");

		// Orphan existing children and cached keys - they'll be recreated lazily
		m_children.clear();
		m_keys.clear();

		// Update table and attach it to userdata if already created
		m_table = newTable;
		if (m_luaUserdata.valid())
		{
			attachRawTableAsUservalueTo(m_luaUserdata);
		}
	}

	void SolLuaDataModelProxy::markDisposed()
	{
		for (auto& [_, child] : m_children)
		{
			child.markDisposed();
		}

		m_table = sol::table(m_table.lua_state(), sol::create);
		if (m_luaUserdata.valid())
		{
			// Point any external references at the now-empty table so reads return nil...
			attachRawTableAsUservalueTo(m_luaUserdata);
			// ...then drop our own cached reference. It aliases this proxy's owning
			// shared_ptr, so keeping it would make the model immortal: the C++ object
			// pins a Lua userdata that pins the C++ object back, an unbreakable cycle
			// until lua_close. Releasing it leaves only external Lua refs, so the model
			// is reclaimed once those are collected (the tombstone contract above).
			m_luaUserdata = sol::make_object(m_table.lua_state(), sol::lua_nil);
		}
	}

} // end namespace Rml::SolLua
