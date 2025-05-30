reckless -- Issue a command to the reckless plugin manager utility
==================================================================

SYNOPSIS
--------

**reckless** *command* [*target/subcommand*] [*target*] 

DESCRIPTION
-----------

The **reckless** RPC starts a reckless process with the *command* and *target* provided.  Node configuration, network, and lightning direrctory are automatically passed to the reckless utility.

- **command** (string) (one of "install", "uninstall", "search", "enable", "disable", "source", "--version"): Determines which command to pass to reckless
   - *command* **install** takes a *plugin\_name* to search for and install a named plugin.
   - *command* **uninstall** takes a *plugin\_name* and attempts to uninstall a plugin of the same name.
   - *command* **search** takes a *plugin\_name* to search for a named plugin.
 ...
- **target/subcommand** (one of, optional): Target of a reckless command or a subcommand.:
  - (string)
  - (array)
- **target** (one of, optional): *name* of a plugin to install/uninstall/search/enable/disable or source to add/remove.:
  - (string)
  - (array)

RETURN VALUE
------------

On success, an object is returned, containing:

- **result** (array of strings): Output of the requested reckless command.:
  - (string, optional)
- **log** (array of strings): Verbose log entries of the requested reckless command.:
  - (string, optional)

AUTHOR
------

Alex Myers <<alex@endothermic.dev>> is mainly responsible.

SEE ALSO
--------

reckless(1)

RESOURCES
---------

Main web site: <https://github.com/ElementsProject/lightning>

EXAMPLES
--------


**Example 1**: 

Request:
```shell
lightning-cli reckless -k "command"="search" "target/subcommand"="backup"
```
```json
{
  "id": "example:reckless#1",
  "method": "reckless",
  "params": {
    "command": "search",
    "target/subcommand": "backup"
  }
}
```

Response:
```json
{
  "result": [
    "https://github.com/lightningd/plugins"
  ],
  "log": [
    "DEBUG: Warning: Reckless requires write access",
    "DEBUG: fetching from gh API: https://api.github.com/repos/lightningd/plugins/contents/",
    "DEBUG: fetching from gh API: https://api.github.com/repos/lightningd/plugins/git/trees/294f93d7060799439c994daa84f534c4d1458325",
    "INFO: found backup in source: https://github.com/lightningd/plugins",
    "DEBUG: entry: None",
    "DEBUG: sub-directory: backup"
  ]
}
```

**Example 2**: 

Request:
```shell
lightning-cli reckless -k "command"="install" "target/subcommand"='["summars", "currecyrate"]'
```
```json
{
  "id": "example:reckless#2",
  "method": "reckless",
  "params": {
    "command": "install",
    "target/subcommand": [
      "summars",
      "currecyrate"
    ]
  }
}
```

Response:
```json
{
  "result": [
    "/tmp/l1/reckless/summars",
    "/tmp/l1/reckless/currencyrate"
  ],
  "log": [
    "DEBUG: Searching for summars",
    "DEBUG: fetching from gh API: https://api.github.com/repos/lightningd/plugins/contents/",
    "DEBUG: fetching from gh API: https://api.github.com/repos/lightningd/plugins/git/trees/294f93d7060799439c994daa84f534c4d1458325",
    "INFO: found summars in source: https://github.com/lightningd/plugins",
    "DEBUG: entry: None",
    "DEBUG: sub-directory: summars",
    "DEBUG: Retrieving summars from https://github.com/lightningd/plugins",
    "DEBUG: Install requested from InstInfo(summars, https://github.com/lightningd/plugins, None, None, None, summars).",
    "INFO: cloning Source.GITHUB_REPO InstInfo(summars, https://github.com/lightningd/plugins, None, None, None, summars)",
    "DEBUG: cloned_src: InstInfo(summars, /tmp/reckless-726255950dyifh_fh/clone, None, Cargo.toml, Cargo.toml, summars/summars)",
    "DEBUG: using latest commit of default branch",
    "DEBUG: checked out HEAD: 5e449468bd57db7d0f33178fe0dc867e0da94133",
    "DEBUG: using installer rust",
    "DEBUG: creating /tmp/l1/reckless/summars",
    "DEBUG: creating /tmp/l1/reckless/summars/source",
    "DEBUG: copying /tmp/reckless-726255950dyifh_fh/clone/summars/summars tree to /tmp/l1/reckless/summars/source/summars",
    "DEBUG: linking source /tmp/l1/reckless/summars/source/summars/Cargo.toml to /tmp/l1/reckless/summars/Cargo.toml",
    "DEBUG: InstInfo(summars, /tmp/l1/reckless/summars, None, Cargo.toml, Cargo.toml, source/summars)",
    "DEBUG: cargo installing from /tmp/l1/reckless/summars/source/summars",
    "DEBUG: rust project compiled successfully",
    "INFO: plugin installed: /tmp/l1/reckless/summars",
    "DEBUG: activating summars",
    "INFO: summars enabled",
    "DEBUG: Searching for currencyrate",
    "DEBUG: fetching from gh API: https://api.github.com/repos/lightningd/plugins/contents/",
    "DEBUG: fetching from gh API: https://api.github.com/repos/lightningd/plugins/git/trees/294f93d7060799439c994daa84f534c4d1458325",
    "INFO: found currencyrate in source: https://github.com/lightningd/plugins",
    "DEBUG: entry: None",
    "DEBUG: sub-directory: currencyrate",
    "DEBUG: Retrieving currencyrate from https://github.com/lightningd/plugins",
    "DEBUG: Install requested from InstInfo(currencyrate, https://github.com/lightningd/plugins, None, None, None, currencyrate).",
    "INFO: cloning Source.GITHUB_REPO InstInfo(currencyrate, https://github.com/lightningd/plugins, None, None, None, currencyrate)",
    "DEBUG: cloned_src: InstInfo(currencyrate, /tmp/reckless-192564272t478naxn/clone, None, currencyrate.py, requirements.txt, currencyrate/currencyrate)",
    "DEBUG: using latest commit of default branch",
    "DEBUG: checked out HEAD: 5e449468bd57db7d0f33178fe0dc867e0da94133",
    "DEBUG: using installer python3venv",
    "DEBUG: creating /tmp/l1/reckless/currencyrate",
    "DEBUG: creating /tmp/l1/reckless/currencyrate/source",
    "DEBUG: copying /tmp/reckless-192564272t478naxn/clone/currencyrate/currencyrate tree to /tmp/l1/reckless/currencyrate/source/currencyrate",
    "DEBUG: linking source /tmp/l1/reckless/currencyrate/source/currencyrate/currencyrate.py to /tmp/l1/reckless/currencyrate/currencyrate.py",
    "DEBUG: InstInfo(currencyrate, /tmp/l1/reckless/currencyrate, None, currencyrate.py, requirements.txt, source/currencyrate)",
    "DEBUG: configuring a python virtual environment (pip) in /tmp/l1/reckless/currencyrate/.venv",
    "DEBUG: virtual environment created in /tmp/l1/reckless/currencyrate/.venv.",
    "INFO: dependencies installed successfully",
    "DEBUG: virtual environment for cloned plugin: .venv",
    "INFO: plugin installed: /tmp/l1/reckless/currencyrate",
    "DEBUG: activating currencyrate",
    "INFO: currencyrate enabled"
  ]
}
```
