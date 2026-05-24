#include <RmlSolLua_private.h>
#include SOLHPP

#include "plugin/SolLuaDataModel.h"

namespace Rml::SolLua
{
	/// The following functions use uservalue/fenv to query the underlying table,
	/// rather than accessing the proxy itself, to reduce indirection level and improve cache locality.
	/// After native disposal markDisposed() empties the backing table, so these naturally yield empty.
	namespace functions
	{
		static int dataModelPairs(lua_State* L) // [-0, +3]
		{
			lua_getuservalue(L, 1);   // [tbl]
			lua_getglobal(L, "next"); // [tbl, next]
			lua_pushvalue(L, -2);     // [tbl, next, tbl]
			lua_pushnil(L);           // [tbl, next, tbl, nil]
			return 3;
		}

		static int dataModelIpairs(lua_State* L) // [-0, +3]
		{
			lua_getuservalue(L, 1);     // [tbl]
			lua_getglobal(L, "ipairs"); // [tbl, ipairs]
			lua_pushvalue(L, -2);       // [tbl, ipairs, tbl]
			lua_call(L, 1, 3);          // [tbl, iter, tbl, 0]
			return 3;
		}

		static int dataModelLen(lua_State* L) // [-0, +1]
		{
			lua_getuservalue(L, 1); // [tbl]
			lua_len(L, -1);         // [tbl, len]
			return 1;
		}
	} // namespace functions

	void bind_datamodel(sol::state_view& lua)
	{
		// clang-format off
		lua.new_usertype<SolLuaDataModelProxy>("SolLuaDataModelProxy", sol::no_constructor,
			sol::meta_function::index, &SolLuaDataModelProxy::luaIndex,
			sol::meta_function::new_index, &SolLuaDataModelProxy::luaNewIndex,
			sol::meta_function::length, &functions::dataModelLen,
			sol::meta_function::pairs, &functions::dataModelPairs,
			sol::meta_function::ipairs, &functions::dataModelIpairs
		);
		// clang-format on
	}
} // end namespace Rml::SolLua
