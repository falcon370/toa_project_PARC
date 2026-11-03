"""The explain-and-fix agent.

The compiler finds and proves problems. This package turns a finding into a
plain-language explanation and into candidate rewrites of the flagged
function, and then lets the compiler judge every candidate. A language model
may write the words and the candidates, but only the compiler decides what is
shown: the agent never has the final say.

    llm.py        one interface to Gemini, to a local Ollama model, or to no model at all
    templates.py  explanations and fixes that need no model (the fallback, and always a candidate)
    gate.py       the compiler gate: is a rewrite correct, safer, and close to the original?
    agent.py      explain() and fix(), the two things the explorer calls
"""
