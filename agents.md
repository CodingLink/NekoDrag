# Project Guidelines

## Python Environment

- **All `pip` package installations must be done inside a virtual environment (venv).**
- Do not install packages into the system Python or user-level site-packages.
- Prefer creating a `.venv` in the project root when one does not exist.

Example:

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install <package>
```
