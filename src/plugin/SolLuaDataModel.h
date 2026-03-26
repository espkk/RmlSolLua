#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <RmlSolLua_private.h>
#include <RmlUi/Core.h>
#include SOLHPP

namespace Rml::SolLua
{
	class SolLuaDataModel;

	class SolLuaDataModelProxy final : public Rml::VariableDefinition
	{
	public:
		SolLuaDataModelProxy(SolLuaDataModel* datamodel, sol::table table);
		bool Get(void* ptr, Rml::Variant& variant) override;
		bool Set(void* ptr, const Rml::Variant& variant) override;
		int Size(void* ptr) override;
		DataVariable Child(void* ptr, const Rml::DataAddressEntry& address) override;
		StringList ReflectMemberNames() override;

		static sol::object luaIndex(SolLuaDataModelProxy& self, sol::stack_object key, sol::this_state ts);
		static void luaNewIndex(SolLuaDataModelProxy& self, sol::stack_object key, sol::stack_object value, sol::this_state ts);

		sol::table& table()
		{
			return m_table;
		}
		const sol::table& table() const
		{
			return m_table;
		}

		void bind(bool topLevel);
		void rebind(const sol::table& newTable);

	private:
		void cacheUserdata();

		SolLuaDataModel* m_datamodel;
		sol::table m_table;

		std::unique_ptr<Rml::VariableDefinition> m_selfAsScalar;

		// Children proxies for nested tables
		std::unordered_map<std::string, SolLuaDataModelProxy> m_children;

		// Store keys of non-table values in a set just to keep alive the strings
		std::unordered_set<std::string> m_keys;

		// Not string_view to avoid transient copy since Rml expects String&
		const std::string* m_topLevelKey = nullptr;

		// Cached Lua userdata for this proxy (used by __index to avoid per-call allocation)
		sol::object m_luaUserdata;
	};

	class SolLuaDataModel
	{
	public:
		SolLuaDataModel(const sol::table& model, const Rml::DataModelConstructor& constructor);

		Rml::DataModelConstructor constructor() const;
		SolLuaDataModelProxy& topLevelProxy();

	private:
		Rml::DataModelConstructor m_constructor;
		SolLuaDataModelProxy m_topLevelProxy;
	};

} // end namespace Rml::SolLua
