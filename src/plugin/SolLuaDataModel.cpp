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

	} // namespace

	SolLuaDataModel::SolLuaDataModel(const sol::table& model, const Rml::DataModelConstructor& constructor)
	    : m_constructor(constructor),
	      m_topLevelProxy(this, model)
	{
		m_topLevelProxy.bind(true);
	}

	Rml::DataModelConstructor SolLuaDataModel::constructor() const
	{
		return m_constructor;
	}

	SolLuaDataModelProxy& SolLuaDataModel::topLevelProxy()
	{
		return m_topLevelProxy;
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
			// Pseudo-key: access by index
			int idx;
			std::from_chars_result result = std::from_chars(key + 1, key + std::strlen(key) - 1, idx);
			RMLUI_ASSERT(result.ec == std::errc{} && "RmlUi failed to sanitize user input to be well-formed");
			if (idx < 0 || idx >= m_table.size())
			{
				Rml::Log::Message(Rml::Log::LT_ERROR, "[LUA][ERROR] Data array index out of bounds");
				RMLUI_ERROR;
				return false;
			}
			sol::table_proxy obj = m_table[idx + 1]; // Lua is 1-based
			obj = makeObjectFromVariant(&variant, m_table.lua_state());
		}
		else
		{
			sol::table_proxy obj = m_table[key];
			obj = makeObjectFromVariant(&variant, m_table.lua_state());
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
			// Create child proxy on-demand
			auto [it, inserted] = m_children.try_emplace(skey, m_datamodel, obj.as<sol::table>());
			if (inserted)
			{
				it->second.m_topLevelKey = m_topLevelKey ? m_topLevelKey : &it->first;
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
		if (!m_luaUserdata.valid())
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
		m_luaUserdata = sol::make_object_userdata(L, this);
		attachRawTableAsUservalueTo(m_luaUserdata);
	}

	sol::object SolLuaDataModelProxy::luaIndex(SolLuaDataModelProxy& self, sol::stack_object key, sol::this_state ts)
	{
		std::string skey = key.is<std::string>() ? key.as<std::string>() : std::format("[{}]", key.as<int>() - 1);

		auto it = self.m_children.find(skey);
		if (it != self.m_children.end())
		{
			return it->second.luaUserdata();
		}

		// Raw lookup in the uservalue table (faster equivalent to m_table).
		// Tables are returned as raw Lua tables - proxies are only created when
		// RmlUi accesses the path via Child().
		lua_State* L = ts;
		lua_getuservalue(L, 1); // [tbl]
		key.push(L);            // [tbl, key]
		lua_rawget(L, -2);      // [tbl, value]
		sol::object result(L, -1);
		lua_pop(L, 2); // []
		return result;
	}

	void SolLuaDataModelProxy::luaNewIndex(SolLuaDataModelProxy& self, sol::stack_object key, sol::stack_object value, sol::this_state ts)
	{
		std::string skey = key.is<std::string>() ? key.as<std::string>() : std::format("[{}]", key.as<int>() - 1);

		// Top-level type changes (scalar<->table) are forbidden because RmlUi's
		// BindVariable uses emplace -- bindings cannot be replaced after creation.
		if (self.m_topLevelKey == nullptr && !IsArrayIndex(skey))
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

		// Raw store into the uservalue table (faster equivalent to `m_table`)
		{
			lua_State* L = ts;
			lua_getuservalue(L, 1); // [tbl]
			key.push(L);            // [tbl, key]
			value.push(L);          // [tbl, key, value]
			lua_rawset(L, -3);      // [tbl]
			lua_pop(L, 1);          // []
		}

		if (value.get_type() == sol::type::table)
		{
			auto proxyIt = self.m_children.find(skey);
			if (proxyIt != self.m_children.end())
			{
				// Existing child proxy - rebind with the new table
				proxyIt->second.rebind(value.as<sol::table>());
			}
			else if (self.m_topLevelKey == nullptr && !IsArrayIndex(skey))
			{
				// New top-level table - create proxy and register with RmlUi
				auto [it, inserted] = self.m_children.try_emplace(skey, self.m_datamodel, value.as<sol::table>());
				if (inserted)
				{
					it->second.m_topLevelKey = &it->first;
					self.m_datamodel->constructor().BindCustomDataVariable(
					    skey, Rml::DataVariable(&it->second, nullptr)
					);
				}
			}
			// else: nested - child proxy will be created lazily via Child()

			if (self.m_topLevelKey != nullptr)
			{
				self.m_keys.erase(skey);
			}
		}
		else if (value.get_type() == sol::type::lua_nil)
		{
			if (self.m_topLevelKey == nullptr)
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
				// Nested removal: erase for clean lazy recreation when re-added.
				self.m_children.erase(skey);
				self.m_keys.erase(skey);
			}
		}
		else if (value.get_type() == sol::type::function && self.m_topLevelKey == nullptr && !IsArrayIndex(skey))
		{
			// New top-level event callback (BindEventCallback rejects duplicates)
			self.m_datamodel->constructor().BindEventCallback(
			    skey,
			    [cb = sol::protected_function{value},
			     state = sol::state_view{ts}](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList& varlist)
			    {
				    if (cb.valid())
				    {
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
			    }
			);
			return;
		}
		else
		{
			// Scalar (or nested function)
			if (self.m_topLevelKey != nullptr)
			{
				self.m_children.erase(skey);
			}

			if (self.m_topLevelKey == nullptr && !IsArrayIndex(skey) && !self.m_keys.contains(skey))
			{
				// New top-level scalar - register with RmlUi
				auto [it, _] = self.m_keys.emplace(skey);
				self.m_datamodel->constructor().BindCustomDataVariable(
				    skey, Rml::DataVariable(self.m_selfAsScalar.get(), const_cast<char*>(it->data()))
				);
			}
		}

		self.m_datamodel->constructor().GetModelHandle().DirtyVariable(self.m_topLevelKey ? *self.m_topLevelKey : skey);
	}

	void SolLuaDataModelProxy::bind(bool topLevel)
	{
		// Nested tables are resolved lazily via Child() and luaIndex().
		// Only the top-level table needs eager iteration to register variables/callbacks with RmlUi.
		if (!topLevel)
		{
			return;
		}

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
					// Assign a pseudo-key for numeric indices
					// Assuming it fits into uint64_t to simplify logic
					skey = std::format("[{}]", static_cast<std::uint64_t>(number)); // Lua is 1-based
				}
			}
			if (skey.empty())
			{
				Rml::Log::Message(Log::LT_ERROR, "[LUA][ERROR] Data model key other than non-empty string or non-negative integer is unsupported");
				return;
			}

			// Skip array elements at top level - they are accessed via Child() on the top-level proxy
			if (IsArrayIndex(skey))
			{
				continue;
			}

			if (value.get_type() == sol::type::table)
			{
				// Create child proxy for top-level named tables (required for BindCustomDataVariable).
				// The child proxy's own nested structure is resolved lazily via Child().
				auto childProxyIt = m_children.try_emplace(skey, m_datamodel, value.as<sol::table>());
				RMLUI_ASSERT(childProxyIt.second);
				childProxyIt.first->second.m_topLevelKey = &childProxyIt.first->first;
				m_datamodel->constructor().BindCustomDataVariable(
				    skey, Rml::DataVariable(&childProxyIt.first->second, nullptr)
				);
			}
			else if (value.get_type() == sol::type::function)
			{
				m_datamodel->constructor().BindEventCallback(
				    skey,
				    [cb = sol::protected_function{value},
				     state = sol::state_view{m_table.lua_state()}](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList& varlist)
				    {
					    if (cb.valid())
					    {
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
				    }
				);
			}
			else
			{
				auto it = m_keys.emplace(skey);
				m_datamodel->constructor().BindCustomDataVariable(
				    skey, Rml::DataVariable(m_selfAsScalar.get(), const_cast<char*>(it.first->data()))
				);
			}
		}
	}

	void SolLuaDataModelProxy::rebind(const sol::table& newTable)
	{
		RMLUI_ASSERT(m_topLevelKey != nullptr && "Rebind should never be done on top-level table");

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

} // end namespace Rml::SolLua
