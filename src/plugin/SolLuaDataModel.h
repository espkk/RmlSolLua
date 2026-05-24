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
		static sol::object luaIndex(SolLuaDataModelProxy& self, sol::stack_object key, sol::this_state ts);
		static void luaNewIndex(SolLuaDataModelProxy& self, sol::stack_object key, sol::stack_object value, sol::this_state ts);

		SolLuaDataModelProxy(SolLuaDataModel* datamodel, sol::table table);

		bool Get(void* ptr, Rml::Variant& variant) override;
		bool Set(void* ptr, const Rml::Variant& variant) override;
		int Size(void* ptr) override;
		DataVariable Child(void* ptr, const Rml::DataAddressEntry& address) override;
		StringList ReflectMemberNames() override;

		void attachRawTableAsUservalueTo(sol::object& target) const;
		sol::object& luaUserdata();

		void bind();
		void rebind(const sol::table& newTable);
		void rebindRoot(const sol::table& newTable);
		void markDisposed();

	private:
		bool isTopLevel() const;
		bool isDisposed() const;

		void registerTopLevelTable(const std::string& key, const sol::table& table);
		void registerTopLevelScalar(const std::string& key);
		void registerTopLevelCallback(const std::string& key, const sol::protected_function& func, lua_State* L);

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

	class SolLuaDataModel : public std::enable_shared_from_this<SolLuaDataModel>
	{
	public:
		// Creates a new model or rebinds an existing one for the given (context, name).
		// The returned shared_ptr keeps the model alive; the registry holds another
		// strong ref until OnDataModelDestroy fires.
		static std::shared_ptr<SolLuaDataModel> GetOrCreate(
		    Rml::Context* context, const Rml::String& name, const sol::table& model, const Rml::DataModelConstructor& constructor
		);
		static void NotifyDestroyed(Rml::Context* context, const Rml::String& name);

		SolLuaDataModel(const sol::table& model, const Rml::DataModelConstructor& constructor);

		Rml::DataModelConstructor constructor() const { return m_constructor; }
		SolLuaDataModelProxy& topLevelProxy() { return m_topLevelProxy; }
		bool isDisposed() const { return m_disposed; }

	private:
		bool m_disposed = false;
		Rml::DataModelConstructor m_constructor;
		SolLuaDataModelProxy m_topLevelProxy;
	};

} // end namespace Rml::SolLua
