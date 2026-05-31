/// @file datamodel_test.cpp
/// @brief Integration tests for SolLuaDataModel with RmlUi's data binding system.
///
/// Each test exercises a single concern and, where applicable, verifies both
/// the Lua proxy path (__index/__newindex) and the RmlUi document resolution
/// path (GetVariable -> Child -> Get).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <RmlUi/Core.h>
#include <sol/sol.hpp>

#include <RmlSolLua/RmlSolLua.h>

#include <chrono>
#include <string>

// ---------------------------------------------------------------------------
// Minimal stubs for RmlUi initialization without a real GPU/window
// ---------------------------------------------------------------------------

namespace
{

	class StubSystemInterface final : public Rml::SystemInterface
	{
	public:
		double GetElapsedTime() override
		{
			using namespace std::chrono;
			return duration<double>(steady_clock::now().time_since_epoch()).count();
		}
	};

	class StubRenderInterface final : public Rml::RenderInterface
	{
	public:
		Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override
		{
			return {};
		}
		void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override
		{}
		void ReleaseGeometry(Rml::CompiledGeometryHandle) override
		{}
		Rml::TextureHandle LoadTexture(Rml::Vector2i&, const Rml::String&) override
		{
			return {};
		}
		Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override
		{
			return {};
		}
		void ReleaseTexture(Rml::TextureHandle) override
		{}
		void EnableScissorRegion(bool) override
		{}
		void SetScissorRegion(Rml::Rectanglei) override
		{}
	};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Fixture: Rml + Lua lifetime
// ---------------------------------------------------------------------------

struct RmlLuaFixture
{
	StubSystemInterface systemInterface;
	StubRenderInterface renderInterface;
	sol::state lua;
	Rml::Context* ctx = nullptr;

	RmlLuaFixture()
	{
		Rml::SetSystemInterface(&systemInterface);
		Rml::SetRenderInterface(&renderInterface);
		Rml::Initialise();

		lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table, sol::lib::math);

		sol::state_view view(lua);
		Rml::SolLua::Initialise(&view);

		ctx = Rml::CreateContext("test", Rml::Vector2i(800, 600));
		REQUIRE(ctx != nullptr);

		lua["Context"] = ctx;
	}

	~RmlLuaFixture()
	{
		Rml::Shutdown();
	}

	/// Create a data model from a Lua table literal.
	/// The proxy is stored in the `model` Lua global.
	sol::object openModel(const std::string& modelName, const std::string& tableLiteral)
	{
		std::string script = std::format(
		    R"(
			local raw = {}
			model = Context:OpenDataModel('{}', raw)
			return model
			)",
		    tableLiteral,
		    modelName
		);

		return lua.safe_script(script);
	}
};

// ---------------------------------------------------------------------------
// Helpers for RML document verification
// ---------------------------------------------------------------------------

namespace
{

	Rml::ElementDocument* loadAndUpdate(Rml::Context* ctx, const std::string& rml)
	{
		auto* doc = ctx->LoadDocumentFromMemory(rml);
		REQUIRE(doc != nullptr);
		doc->Show();
		ctx->Update();
		return doc;
	}

	std::string makeRml(const std::string& modelName, const std::string& bodyContent)
	{
		return std::format(
		    R"(<rml><head><title>test</title></head>)"
		    R"(<body data-model="{}">{})</body></rml>)",
		    modelName,
		    bodyContent
		);
	}

	std::string innerRmlOf(Rml::ElementDocument* doc, const std::string& id)
	{
		auto* el = doc->GetElementById(id);
		REQUIRE(el != nullptr);
		Rml::String rml = el->GetInnerRML();
		auto start = rml.find_first_not_of(" \t\n\r");
		auto end = rml.find_last_not_of(" \t\n\r");
		if (start == std::string::npos)
		{
			return {};
		}
		return rml.substr(start, end - start + 1);
	}

} // anonymous namespace

// ===========================================================================
// SMOKE TESTS - basic Lua proxy behavior without RML documents
// ===========================================================================

TEST_CASE("Proxy creation and scalar type support", "[datamodel][smoke]")
{
	// Concern: openModel returns a valid proxy userdata; all scalar types
	// (int, string, double, bool) are readable through __index.
	RmlLuaFixture f;
	sol::object proxy = f.openModel("scalar_types", "{ count = 42, label = 'hello', ratio = 3.14, flag = true }");
	REQUIRE(proxy.valid());

	CHECK(f.lua.safe_script("return model.count").get<int>() == 42);
	CHECK(f.lua.safe_script("return model.label").get<std::string>() == "hello");
	CHECK(f.lua.safe_script("return model.ratio").get<double>() == Catch::Approx(3.14));
	CHECK(f.lua.safe_script("return model.flag").get<bool>() == true);
}

TEST_CASE("Nested table access returns proxy userdata", "[datamodel][smoke]")
{
	// Concern: accessing a nested table through __index returns a userdata
	// proxy (not a raw Lua table), and its fields are readable.
	RmlLuaFixture f;
	f.openModel("nested_proxy", "{ child = { x = 10, y = 20 } }");

	CHECK(f.lua.safe_script("return type(model.child)").get<std::string>() == "userdata");
	CHECK(f.lua.safe_script("return model.child.x").get<int>() == 10);
	CHECK(f.lua.safe_script("return model.child.y").get<int>() == 20);
}

// ===========================================================================
// READING - initial values visible through both Lua and RML
// ===========================================================================

TEST_CASE("Top-level scalars readable in Lua and rendered in document", "[datamodel][read]")
{
	// Concern: values bound at model creation are accessible via Lua __index
	// and resolve correctly in RML {{expressions}}.
	RmlLuaFixture f;
	f.openModel("toplevel_read", "{ score = 42, name = 'hero' }");

	CHECK(f.lua.safe_script("return model.score").get<int>() == 42);
	CHECK(f.lua.safe_script("return model.name").get<std::string>() == "hero");

	auto* doc = loadAndUpdate(f.ctx, makeRml("toplevel_read", R"(<p id="s">{{score}}</p><p id="n">{{name}}</p>)"));

	CHECK(innerRmlOf(doc, "s") == "42");
	CHECK(innerRmlOf(doc, "n") == "hero");
}

TEST_CASE("Nested scalars resolve via Child path in Lua and document", "[datamodel][read]")
{
	// Concern: multi-segment addresses (e.g., player.hp) are resolved by
	// RmlUi's GetVariable -> Child() -> Get() chain.
	RmlLuaFixture f;
	f.openModel("nested_read", "{ player = { hp = 100, name = 'Alice' } }");

	CHECK(f.lua.safe_script("return model.player.hp").get<int>() == 100);
	CHECK(f.lua.safe_script("return model.player.name").get<std::string>() == "Alice");

	auto* doc = loadAndUpdate(f.ctx, makeRml("nested_read", R"(<p id="hp">{{player.hp}}</p><p id="n">{{player.name}}</p>)"));

	CHECK(innerRmlOf(doc, "hp") == "100");
	CHECK(innerRmlOf(doc, "n") == "Alice");
}

TEST_CASE("Deeply nested values accessible through proxy chain", "[datamodel][read]")
{
	// Concern: lazy child proxy creation handles multi-level nesting
	// (Child() called repeatedly for each path segment).
	RmlLuaFixture f;
	f.openModel("deep_read", "{ a = { b = { c = { d = 'deep' } } } }");

	CHECK(f.lua.safe_script("return model.a.b.c.d").get<std::string>() == "deep");

	auto* doc = loadAndUpdate(f.ctx, makeRml("deep_read", R"(<p id="v">{{a.b.c.d}}</p>)"));

	CHECK(innerRmlOf(doc, "v") == "deep");
}

// ===========================================================================
// SCALAR MUTATION
// ===========================================================================

TEST_CASE("Top-level scalar change visible in Lua and document", "[datamodel][change]")
{
	// Concern: __newindex on a top-level scalar updates the backing store;
	// the new value is visible both via Lua __index and in the document
	// after Update().
	RmlLuaFixture f;
	f.openModel("scalar_change", "{ health = 100, greeting = 'hello' }");

	auto* doc = loadAndUpdate(f.ctx, makeRml("scalar_change", R"(<p id="h">{{health}}</p><p id="g">{{greeting}}</p>)"));

	CHECK(innerRmlOf(doc, "h") == "100");

	f.lua.safe_script("model.health = 75");
	f.lua.safe_script("model.greeting = 'world'");
	f.ctx->Update();

	CHECK(f.lua.safe_script("return model.health").get<int>() == 75);
	CHECK(f.lua.safe_script("return model.greeting").get<std::string>() == "world");
	CHECK(innerRmlOf(doc, "h") == "75");
	CHECK(innerRmlOf(doc, "g") == "world");
}

TEST_CASE("Nested scalar change visible in Lua and document", "[datamodel][change]")
{
	// Concern: __newindex on a nested proxy scalar propagates through the
	// proxy chain; both Lua and RmlUi see the updated value.
	RmlLuaFixture f;
	f.openModel("nested_change", "{ stats = { str = 10, dex = 8 } }");

	auto* doc = loadAndUpdate(f.ctx, makeRml("nested_change", R"(<p id="str">{{stats.str}}</p><p id="dex">{{stats.dex}}</p>)"));

	f.lua.safe_script("model.stats.str = 20");
	f.lua.safe_script("model.stats.dex = 15");
	f.ctx->Update();

	CHECK(f.lua.safe_script("return model.stats.str").get<int>() == 20);
	CHECK(f.lua.safe_script("return model.stats.dex").get<int>() == 15);
	CHECK(innerRmlOf(doc, "str") == "20");
	CHECK(innerRmlOf(doc, "dex") == "15");
}

TEST_CASE("Rapid scalar updates converge to final value", "[datamodel][change]")
{
	// Concern: many sequential writes to the same key don't corrupt state;
	// document shows the last written value after a single Update().
	RmlLuaFixture f;
	f.openModel("rapid_updates", "{ counter = 0 }");

	auto* doc = loadAndUpdate(f.ctx, makeRml("rapid_updates", R"(<p id="c">{{counter}}</p>)"));

	for (int i = 1; i <= 10; ++i)
	{
		f.lua.safe_script("model.counter = " + std::to_string(i));
	}
	f.ctx->Update();

	CHECK(f.lua.safe_script("return model.counter").get<int>() == 10);
	CHECK(innerRmlOf(doc, "c") == "10");
}

// ===========================================================================
// TABLE REBIND
// ===========================================================================

TEST_CASE("Replacing entire table rebinds children in Lua and document", "[datamodel][rebind]")
{
	// Concern: assigning a new table to an existing table key calls rebind(),
	// clears old m_keys/m_children, and new children are visible everywhere.
	RmlLuaFixture f;
	f.openModel("table_rebind", "{ enemy = { name = 'goblin', hp = 30 } }");

	auto* doc = loadAndUpdate(f.ctx, makeRml("table_rebind", R"(<p id="n">{{enemy.name}}</p><p id="hp">{{enemy.hp}}</p>)"));

	CHECK(innerRmlOf(doc, "n") == "goblin");

	f.lua.safe_script("model.enemy = { name = 'dragon', hp = 500 }");
	f.ctx->Update();

	CHECK(f.lua.safe_script("return model.enemy.name").get<std::string>() == "dragon");
	CHECK(f.lua.safe_script("return model.enemy.hp").get<int>() == 500);
	CHECK(innerRmlOf(doc, "n") == "dragon");
	CHECK(innerRmlOf(doc, "hp") == "500");
}

TEST_CASE("Repeated table rebind across frames updates document each time", "[datamodel][rebind]")
{
	// Concern: rebind() correctly clears m_keys each time, so Child() lazily
	// recreates entries pointing to new data on every frame.
	RmlLuaFixture f;
	f.openModel("frame_rebind", R"({
		fps = { average = 0, min = 0, max = 0 }
	})");

	auto* doc = loadAndUpdate(f.ctx, makeRml("frame_rebind", R"(<p id="avg">{{fps.average}}</p><p id="mn">{{fps.min}}</p><p id="mx">{{fps.max}}</p>)"));

	CHECK(innerRmlOf(doc, "avg") == "0");

	f.lua.safe_script("model.fps = { average = 60, min = 55, max = 65 }");
	f.ctx->Update();
	CHECK(innerRmlOf(doc, "avg") == "60");
	CHECK(innerRmlOf(doc, "mn") == "55");
	CHECK(innerRmlOf(doc, "mx") == "65");

	f.lua.safe_script("model.fps = { average = 120, min = 100, max = 144 }");
	f.ctx->Update();
	CHECK(innerRmlOf(doc, "avg") == "120");
	CHECK(innerRmlOf(doc, "mn") == "100");
	CHECK(innerRmlOf(doc, "mx") == "144");
}

TEST_CASE("Lua refs become inert after native data model removal", "[datamodel][lifetime]")
{
	// Concern: Lua may hold model refs after RmlUi destroys the native model.
	// Reads should return nil/empty and writes should not touch native handles.
	RmlLuaFixture f;
	f.openModel("destroyed_model", "{ player = { name = 'alive' }, score = 7 }");

	f.lua.safe_script("old_model = model; old_player = model.player");
	REQUIRE(f.ctx->RemoveDataModel("destroyed_model"));

	CHECK(f.lua.safe_script("return old_model.score").get<sol::object>().get_type() == sol::type::lua_nil);
	CHECK(f.lua.safe_script("return old_player.name").get<sol::object>().get_type() == sol::type::lua_nil);
	CHECK(f.lua.safe_script("return #old_player").get<int>() == 0);
	CHECK_NOTHROW(f.lua.safe_script("old_model.score = 8; old_player.name = 'dead'"));
}

TEST_CASE("Nested Lua refs stay valid without root proxy until native removal", "[datamodel][lifetime]")
{
	// Concern: the C++ model is owned by the Lua-created data model registry,
	// not only by the root Lua proxy.
	RmlLuaFixture f;
	f.openModel("nested_handle_lifetime", "{ player = { name = 'alive' }, score = 7 }");

	f.lua.safe_script("old_player = model.player; model = nil; collectgarbage(); collectgarbage()");
	CHECK(f.lua.safe_script("return old_player.name").get<std::string>() == "alive");

	REQUIRE(f.ctx->RemoveDataModel("nested_handle_lifetime"));
	CHECK(f.lua.safe_script("return old_player.name").get<sol::object>().get_type() == sol::type::lua_nil);
	CHECK_NOTHROW(f.lua.safe_script("old_player.name = 'dead'"));
}

// ===========================================================================
// LATE BINDING -- variables added after model creation
// ===========================================================================

TEST_CASE("Dynamically added top-level scalar accessible in Lua and document", "[datamodel][late-binding]")
{
	// Concern: a variable that didn't exist at bind() time can be added via
	// __newindex and subsequently resolved by RmlUi.
	RmlLuaFixture f;
	f.openModel("late_scalar", "{ existing = 1 }");

	auto* doc = loadAndUpdate(f.ctx, makeRml("late_scalar", R"(<p id="e">{{existing}}</p><p id="l">{{added}}</p>)"));

	CHECK(innerRmlOf(doc, "e") == "1");

	f.lua.safe_script("model.added = 'hello'");
	f.ctx->Update();

	CHECK(f.lua.safe_script("return model.added").get<std::string>() == "hello");
	CHECK(innerRmlOf(doc, "l") == "hello");
}

TEST_CASE("Dynamically added top-level table accessible in Lua and document", "[datamodel][late-binding]")
{
	// Concern: a table added after model creation is traversable via both
	// Lua proxy chain and RmlUi's Child() path.
	RmlLuaFixture f;
	f.openModel("late_table", "{ x = 1 }");

	auto* doc = loadAndUpdate(f.ctx, makeRml("late_table", R"(<p id="v">{{info.label}}</p>)"));

	f.lua.safe_script("model.info = { label = 'dynamic' }");
	f.ctx->Update();

	CHECK(f.lua.safe_script("return model.info.label").get<std::string>() == "dynamic");
	CHECK(innerRmlOf(doc, "v") == "dynamic");
}

TEST_CASE("Adding key to existing nested table visible in Lua and document", "[datamodel][late-binding]")
{
	// Concern: __newindex on a child proxy can add new keys to an existing
	// nested table, including initially empty sub-tables.
	RmlLuaFixture f;
	f.openModel("late_nested_key", "{ nested = { animal = 'dog' }, container = {} }");

	auto* doc = loadAndUpdate(f.ctx, makeRml("late_nested_key", R"(<p id="a">{{nested.animal}}</p><p id="l">{{nested.late}}</p><p id="c">{{container.child}}</p>)"));

	CHECK(innerRmlOf(doc, "a") == "dog");

	f.lua.safe_script("model.nested.late = 'fox'");
	f.lua.safe_script("model.container.child = 'added'");
	f.ctx->Update();

	CHECK(f.lua.safe_script("return model.nested.late").get<std::string>() == "fox");
	CHECK(f.lua.safe_script("return model.container.child").get<std::string>() == "added");
	CHECK(innerRmlOf(doc, "l") == "fox");
	CHECK(innerRmlOf(doc, "c") == "added");
}

// ===========================================================================
// REMOVAL - nil assignment
// ===========================================================================

TEST_CASE("Nil assignment erases top-level scalar", "[datamodel][removal]")
{
	// Concern: setting a top-level scalar to nil removes it from the
	// backing table; subsequent reads return nil.
	RmlLuaFixture f;
	f.openModel("nil_scalar", "{ temp = 42 }");

	f.lua.safe_script("model.temp = nil");

	CHECK(f.lua.safe_script("return model.temp").get<sol::object>().get_type() == sol::type::lua_nil);
}

TEST_CASE("Nil on table invalidates descendants; re-set restores in document", "[datamodel][removal]")
{
	// Concern: setting a table to nil calls rebind() with an empty table,
	// making all descendants return nil and render empty. Re-setting with
	// new data restores everything through both Lua and RmlUi paths.
	RmlLuaFixture f;
	f.openModel("nil_table_restore", "{ data = { val = 'original' } }");

	auto* doc = loadAndUpdate(f.ctx, makeRml("nil_table_restore", R"(<p id="v">{{data.val}}</p>)"));

	CHECK(innerRmlOf(doc, "v") == "original");

	f.lua.safe_script("model.data = nil");
	f.ctx->Update();

	CHECK(f.lua.safe_script("return model.data.val").get<sol::object>().get_type() == sol::type::lua_nil);
	CHECK(innerRmlOf(doc, "v") == "");

	f.lua.safe_script("model.data = { val = 'restored' }");
	f.ctx->Update();

	CHECK(f.lua.safe_script("return model.data.val").get<std::string>() == "restored");
	CHECK(innerRmlOf(doc, "v") == "restored");
}

TEST_CASE("Nil on nested table removes it; re-add restores", "[datamodel][removal]")
{
	// Concern: nil assignment at a non-top-level removes the child proxy
	// entirely (not just rebind to empty). Re-adding creates a new proxy.
	RmlLuaFixture f;
	f.openModel("nil_nested", "{ outer = { inner = { val = 1 } } }");

	CHECK(f.lua.safe_script("return model.outer.inner.val").get<int>() == 1);

	f.lua.safe_script("model.outer.inner = nil");
	CHECK(f.lua.safe_script("return model.outer.inner").get<sol::object>().get_type() == sol::type::lua_nil);

	f.lua.safe_script("model.outer.inner = { val = 99 }");
	CHECK(f.lua.safe_script("return model.outer.inner.val").get<int>() == 99);
}

TEST_CASE("Proxy identity preserved after table removal and re-set", "[datamodel][removal]")
{
	// Concern: the top-level table proxy uses rebind() (not destroy + recreate),
	// so the Lua userdata identity (pointer) stays the same across nil + re-set.
	RmlLuaFixture f;
	f.openModel("proxy_stability", "{ data = { x = 1 } }");

	f.lua.safe_script("ref_before = model.data");
	f.lua.safe_script("model.data = nil");
	f.lua.safe_script("model.data = { x = 2 }");
	f.lua.safe_script("ref_after = model.data");

	CHECK(f.lua.safe_script("return ref_before == ref_after").get<bool>() == true);
}

// ===========================================================================
// TYPE CHANGES
// ===========================================================================

TEST_CASE("Scalar-to-table and table-to-scalar conversion at nested level", "[datamodel][type-change]")
{
	// Concern: at a nested level, __newindex can freely change a key's type
	// between scalar and table because the parent proxy handles rebinding.
	RmlLuaFixture f;
	f.openModel("nested_type_change", "{ outer = { val = 10 } }");

	f.lua.safe_script("model.outer.val = { sub = 'changed' }");
	CHECK(f.lua.safe_script("return model.outer.val.sub").get<std::string>() == "changed");

	f.lua.safe_script("model.outer.val = 42");
	CHECK(f.lua.safe_script("return model.outer.val").get<int>() == 42);
}

// ===========================================================================
// ARRAYS
// ===========================================================================

TEST_CASE("Array length, ipairs, and .size in document", "[datamodel][array]")
{
	// Concern: numeric-indexed tables support __len, ipairs iteration,
	// and RmlUi's .size pseudo-property for document rendering.
	RmlLuaFixture f;
	f.openModel("array_basics", "{ items = { 'sword', 'shield', 'potion' } }");

	CHECK(f.lua.safe_script("return #model.items").get<int>() == 3);

	sol::table t = f.lua.safe_script(R"(
		local result = {}
		for i, v in ipairs(model.items) do
			result[i] = v
		end
		return result
	)");
	CHECK(t[1].get<std::string>() == "sword");
	CHECK(t[2].get<std::string>() == "shield");
	CHECK(t[3].get<std::string>() == "potion");

	auto* doc = loadAndUpdate(f.ctx, makeRml("array_basics", R"(<p id="sz">{{items.size}}</p>)"));
	CHECK(innerRmlOf(doc, "sz") == "3");
}

TEST_CASE("Array of tables: index access and data-for iteration", "[datamodel][array]")
{
	// Concern: arrays of tables support 1-based index access in Lua and
	// data-for loop rendering in RML documents.
	RmlLuaFixture f;
	f.openModel("array_of_tables", R"({
		entries = {
			{ name = 'alpha' },
			{ name = 'beta' },
			{ name = 'gamma' }
		}
	})");

	CHECK(f.lua.safe_script("return #model.entries").get<int>() == 3);
	CHECK(f.lua.safe_script("return model.entries[1].name").get<std::string>() == "alpha");
	CHECK(f.lua.safe_script("return model.entries[3].name").get<std::string>() == "gamma");

	auto* doc = loadAndUpdate(f.ctx, makeRml("array_of_tables", R"(<div id="list"><p data-for="e : entries">{{e.name}}</p></div>)"));

	Rml::String listRml = doc->GetElementById("list")->GetInnerRML();
	CHECK(listRml.find("alpha") != std::string::npos);
	CHECK(listRml.find("beta") != std::string::npos);
	CHECK(listRml.find("gamma") != std::string::npos);
}

TEST_CASE("Array mutation: element replacement and dynamic extension", "[datamodel][array]")
{
	// Concern: individual array elements can be overwritten; new elements
	// can be appended, increasing the array length.
	RmlLuaFixture f;
	f.openModel("array_mutation", "{ list = { 10, 20, 30 } }");

	f.lua.safe_script("model.list[2] = 99");
	CHECK(f.lua.safe_script("return model.list[2]").get<int>() == 99);

	f.lua.safe_script("model.list[4] = 40");
	CHECK(f.lua.safe_script("return #model.list").get<int>() == 4);
	CHECK(f.lua.safe_script("return model.list[4]").get<int>() == 40);
}

TEST_CASE("Array element change visible in document via data-for", "[datamodel][array]")
{
	// Concern: replacing an array element triggers document update
	// for data-for rendered content after Update().
	RmlLuaFixture f;
	f.openModel("array_doc_change", R"({
		items = { { name = 'A' }, { name = 'B' }, { name = 'C' } }
	})");

	auto* doc = loadAndUpdate(f.ctx, makeRml("array_doc_change", R"(<div id="list"><span data-for="item : items">{{item.name}} </span></div>)"));

	Rml::String before = doc->GetElementById("list")->GetInnerRML();
	CHECK(before.find("A") != std::string::npos);

	f.lua.safe_script("model.items[1] = { name = 'Z' }");
	f.ctx->Update();

	Rml::String after = doc->GetElementById("list")->GetInnerRML();
	CHECK(after.find("Z") != std::string::npos);
	CHECK(after.find("B") != std::string::npos);
	CHECK(after.find("C") != std::string::npos);
}

// ===========================================================================
// FUNCTIONS
// ===========================================================================

TEST_CASE("Function binding: initial and dynamically added", "[datamodel][function]")
{
	// Concern: Lua functions in the data table are bound as event callbacks;
	// they're accessible via __index and can be added dynamically.
	RmlLuaFixture f;
	f.openModel("func_binding", "{ callback = function() return 'initial' end }");

	CHECK(f.lua.safe_script("return type(model.callback)").get<std::string>() == "function");

	f.lua.safe_script("model.on_click = function() return 'dynamic' end");
	CHECK(f.lua.safe_script("return type(model.on_click)").get<std::string>() == "function");
}

TEST_CASE("Re-opening with a new callback body replaces the bound function", "[datamodel][hot-reload]")
{
	// Concern: hot-reload re-runs OpenDataModel with a fresh src containing a
	// new closure for the same event-callback key. RmlUi's BindEventCallback
	// uses emplace and can't replace, so we install an indirection lambda once
	// and swap the stored function on rebind. After re-open, dispatching the
	// event should fire the NEW function.
	RmlLuaFixture f;
	f.lua.safe_script("counter = 0");
	f.lua.safe_script(R"(
		local src = { my_cb = function() counter = counter + 1 end }
		Context:OpenDataModel('reopen_cb', src)
	)");

	auto* doc = loadAndUpdate(f.ctx, makeRml("reopen_cb", R"(<button id="b" data-event-click="my_cb()">x</button>)"));
	auto* btn = doc->GetElementById("b");
	REQUIRE(btn != nullptr);

	btn->Click();
	f.ctx->Update();
	CHECK(f.lua.safe_script("return counter").get<int>() == 1);

	// Hot-reload: re-open with a callback that increments by 10 instead of 1.
	f.lua.safe_script(R"(
		local src = { my_cb = function() counter = counter + 10 end }
		Context:OpenDataModel('reopen_cb', src)
	)");

	btn->Click();
	f.ctx->Update();
	CHECK(f.lua.safe_script("return counter").get<int>() == 11);
}

TEST_CASE("Re-opening without a previously bound callback makes it inert", "[datamodel][hot-reload]")
{
	// Concern: if a callback key is dropped from the new src on rebind, the
	// RmlUi binding still exists (immutable) but should no-op rather than
	// invoke the stale closure.
	RmlLuaFixture f;
	f.lua.safe_script("counter = 0");
	f.lua.safe_script(R"(
		Context:OpenDataModel('drop_cb', { my_cb = function() counter = counter + 1 end })
	)");

	auto* doc = loadAndUpdate(f.ctx, makeRml("drop_cb", R"(<button id="b" data-event-click="my_cb()">x</button>)"));
	auto* btn = doc->GetElementById("b");
	REQUIRE(btn != nullptr);

	btn->Click();
	f.ctx->Update();
	REQUIRE(f.lua.safe_script("return counter").get<int>() == 1);

	// Re-open with src missing my_cb entirely.
	f.lua.safe_script("Context:OpenDataModel('drop_cb', { other = 1 })");

	btn->Click();
	f.ctx->Update();
	CHECK(f.lua.safe_script("return counter").get<int>() == 1);
}

TEST_CASE("Re-opening keeps existing Lua proxy references bound to the new source table", "[datamodel][hot-reload]")
{
	// Concern: the UI runtime keeps stable module/view objects across Lua hot
	// reloads. Any Lua code holding old root or nested model refs must observe
	// the new table after OpenDataModel re-runs, not stale data from before reload.
	RmlLuaFixture f;
	f.lua.safe_script(R"(
		model = Context:OpenDataModel('stable_reopen', {
			title = 'old',
			panel = { label = 'before' },
		})
		old_model = model
		old_panel = model.panel
	)");

	f.lua.safe_script(R"(
		reopened = Context:OpenDataModel('stable_reopen', {
			title = 'new',
			panel = { label = 'after' },
		})
	)");

	CHECK(f.lua.safe_script("return old_model == reopened").get<bool>() == true);
	CHECK(f.lua.safe_script("return old_model.title").get<std::string>() == "new");
	CHECK(f.lua.safe_script("return old_panel.label").get<std::string>() == "after");
}

// ===========================================================================
// BOOLEAN / DATA-IF
// ===========================================================================

TEST_CASE("Boolean value controls data-if element visibility", "[datamodel][boolean]")
{
	// Concern: boolean scalars work with RmlUi's data-if directive;
	// toggling the value hides/shows the element after Update().
	RmlLuaFixture f;
	f.openModel("bool_toggle", "{ visible = true }");

	auto* doc = loadAndUpdate(f.ctx, makeRml("bool_toggle", R"(<p id="vis" data-if="visible">shown</p>)"));

	auto* el = doc->GetElementById("vis");
	REQUIRE(el != nullptr);
	CHECK(el->IsVisible());

	f.lua.safe_script("model.visible = false");
	f.ctx->Update();

	CHECK_FALSE(el->IsVisible());
}

// ===========================================================================
// ITERATION
// ===========================================================================

TEST_CASE("Pairs iteration enumerates all keys", "[datamodel][iteration]")
{
	// Concern: the __pairs metamethod correctly yields all keys in the
	// backing table, enabling Lua `for k, v in pairs(model)` loops.
	RmlLuaFixture f;
	f.openModel("pairs_enum", "{ alpha = 1, bravo = 2, charlie = 3 }");

	auto result = f.lua.safe_script(R"(
		local keys = {}
		for k, v in pairs(model) do
			keys[#keys + 1] = k
		end
		table.sort(keys)
		return table.concat(keys, ',')
	)");
	CHECK(result.get<std::string>() == "alpha,bravo,charlie");
}

// ===========================================================================
// EDGE CASES
// ===========================================================================

TEST_CASE("Nonexistent keys return nil at any nesting level", "[datamodel][edge]")
{
	// Concern: accessing keys that were never set returns nil without error,
	// at both the top level and within nested proxies.
	RmlLuaFixture f;
	f.openModel("nil_access", "{ tbl = { a = 1 } }");

	CHECK(f.lua.safe_script("return model.missing").get<sol::object>().get_type() == sol::type::lua_nil);
	CHECK(f.lua.safe_script("return model.tbl.missing").get<sol::object>().get_type() == sol::type::lua_nil);
}

TEST_CASE("Nil intermediate in chain renders empty in document", "[datamodel][edge]")
{
	// Concern: when RmlUi resolves a multi-segment address like {{a.b.c}}
	// and an intermediate key (b) does not exist, Child() returns an empty
	// DataVariable. The document should render the expression as empty
	// without crashing. Same for a missing leaf.
	RmlLuaFixture f;
	f.openModel("nil_chain", "{ a = { b = { c = 'found' } } }");

	auto* doc = loadAndUpdate(f.ctx, makeRml("nil_chain", R"(<p id="hit">{{a.b.c}}</p>)"
	                                                      R"(<p id="missing_leaf">{{a.b.missing}}</p>)"
	                                                      R"(<p id="missing_mid">{{a.missing.c}}</p>)"
	                                                      R"(<p id="missing_top">{{nope.b.c}}</p>)"));

	CHECK(innerRmlOf(doc, "hit") == "found");
	CHECK(innerRmlOf(doc, "missing_leaf") == "");
	CHECK(innerRmlOf(doc, "missing_mid") == "");
	CHECK(innerRmlOf(doc, "missing_top") == "");
}

TEST_CASE("Empty table model supports dynamic addition", "[datamodel][edge]")
{
	// Concern: a model bound to an empty table still works; variables can
	// be added dynamically and read back.
	RmlLuaFixture f;
	f.openModel("empty_model", "{}");

	CHECK(f.lua.safe_script("return #model").get<int>() == 0);

	f.lua.safe_script("model.x = 42");
	CHECK(f.lua.safe_script("return model.x").get<int>() == 42);
}

// ===========================================================================
// ISOLATION
// ===========================================================================

TEST_CASE("Concurrent models are independent in Lua and document", "[datamodel][isolation]")
{
	// Concern: multiple data models bound to the same context don't
	// interfere with each other; modifications to one leave others unchanged.
	RmlLuaFixture f;

	f.lua.safe_script(R"(
		model_a = Context:OpenDataModel('iso_alpha', { val = 'alpha' })
		model_b = Context:OpenDataModel('iso_beta', { val = 'beta' })
	)");

	auto* docA = loadAndUpdate(f.ctx, makeRml("iso_alpha", R"(<p id="v">{{val}}</p>)"));
	auto* docB = loadAndUpdate(f.ctx, makeRml("iso_beta", R"(<p id="v">{{val}}</p>)"));

	CHECK(innerRmlOf(docA, "v") == "alpha");
	CHECK(innerRmlOf(docB, "v") == "beta");

	f.lua.safe_script("model_a.val = 'changed'");
	f.ctx->Update();

	CHECK(innerRmlOf(docA, "v") == "changed");
	CHECK(innerRmlOf(docB, "v") == "beta");
}

// ===========================================================================
// RMLUI API INTEGRATION
// ===========================================================================

TEST_CASE("DirtyVariable works for statically and dynamically bound variables", "[datamodel][integration]")
{
	// Concern: calling DirtyVariable() through the RmlUi data model handle
	// doesn't crash for variables bound at creation or added dynamically.
	RmlLuaFixture f;
	f.openModel("dirty_api", "{ score = 42, player = { name = 'hero' } }");

	auto constructor = f.ctx->GetDataModel("dirty_api");
	REQUIRE(constructor);

	auto handle = constructor.GetModelHandle();
	REQUIRE(handle);

	handle.DirtyVariable("score");
	handle.DirtyVariable("player");

	f.lua.safe_script("model.dynamic_var = 100");
	handle.DirtyVariable("dynamic_var");
}
