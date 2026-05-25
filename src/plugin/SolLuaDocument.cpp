#include <RmlSolLua_private.h>
#include <RmlUi/Core.h>
#include SOLHPP

#include "SolLuaDocument.h"

namespace Rml::SolLua
{

	sol::protected_function_result ErrorHandler(lua_State*, sol::protected_function_result pfr)
	{
		if (!pfr.valid())
		{
			sol::error err = pfr;
			Rml::Log::Message(Rml::Log::LT_ERROR, "[LUA][ERROR] %s", err.what());
		}
		return pfr;
	}

	//-----------------------------------------------------

	SolLuaDocument::SolLuaDocument(sol::state_view state, const Rml::String& tag, const Rml::String& lua_env_identifier)
	    : m_state(state), ElementDocument(tag), m_environment(state, sol::create, state.globals()), m_lua_env_identifier(lua_env_identifier)
	{
	}

	void SolLuaDocument::LoadInlineScript(const Rml::String& content, const Rml::String& source_path, int source_line)
	{
		auto* context = GetContext();

		// A context isn't present in the case of reloading stylesheets which reprocesses the whole head section.
		if (context == nullptr)
			return;

		Rml::String buffer{"--"};
		buffer.append("[");
		buffer.append(context->GetName());
		buffer.append("][");
		buffer.append(GetSourceURL());
		buffer.append("]:");
		buffer.append(Rml::ToString(source_line));
		buffer.append("\n");
		buffer.append(content);

		if (!m_lua_env_identifier.empty())
			m_environment[m_lua_env_identifier] = GetId();

		m_state.safe_script(buffer, m_environment, ErrorHandler);
	}

	void SolLuaDocument::LoadExternalScript(const String& source_path)
	{
		if (!m_lua_env_identifier.empty())
			m_environment[m_lua_env_identifier] = GetId();

		// Fix the path if a leading colon has been replaced with a pipe.
		String fixed_path = StringUtilities::Replace(source_path, '|', ':');

		// Run the script. If it returns a function, that function is the document's
		// entry point: we call it with the document, so `document` is an explicit
		// parameter rather than an injected global. Re-running the script (e.g. for
		// hot reload) re-invokes the entry, which re-opens the data model and lets the
		// C++ side rebind it. Scripts that only run side effects at file scope (and
		// return nothing) still work unchanged.
		auto result = m_state.safe_script_file(fixed_path, m_environment, ErrorHandler);
		if (!result.valid())
			return;

		sol::object entry = result;
		if (entry.get_type() != sol::type::function)
			return;

		sol::protected_function open = entry;
		auto open_result = open(this);
		if (!open_result.valid())
			ErrorHandler(m_state.lua_state(), std::move(open_result));
	}

	sol::protected_function_result SolLuaDocument::RunLuaScript(const Rml::String& script)
	{
		if (!m_lua_env_identifier.empty())
			m_environment[m_lua_env_identifier] = GetId();

		return m_state.safe_script(script, m_environment, ErrorHandler);
	}

} // namespace Rml::SolLua
