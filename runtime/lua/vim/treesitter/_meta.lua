---@meta
error('Cannot require a meta file')

---@class TSNode: userdata
---@field id fun(self: TSNode): string
---@field tree fun(self: TSNode): TSTree
---@field range fun(self: TSNode, include_bytes: false?): integer, integer, integer, integer
---@field range fun(self: TSNode, include_bytes: true): integer, integer, integer, integer, integer, integer
---@field start fun(self: TSNode): integer, integer, integer
---@field end_ fun(self: TSNode): integer, integer, integer
---@field type fun(self: TSNode): string
---@field symbol fun(self: TSNode): integer
---@field named fun(self: TSNode): boolean
---@field missing fun(self: TSNode): boolean
---@field extra fun(self: TSNode): boolean
---@field child_count fun(self: TSNode): integer
---@field named_child_count fun(self: TSNode): integer
---@field child fun(self: TSNode, index: integer): TSNode?
---@field named_child fun(self: TSNode, index: integer): TSNode?
---@field descendant_for_range fun(self: TSNode, start_row: integer, start_col: integer, end_row: integer, end_col: integer): TSNode?
---@field named_descendant_for_range fun(self: TSNode, start_row: integer, start_col: integer, end_row: integer, end_col: integer): TSNode?
---@field parent fun(self: TSNode): TSNode?
---@field child_containing_descendant fun(self: TSNode, descendant: TSNode): TSNode?
---@field next_sibling fun(self: TSNode): TSNode?
---@field prev_sibling fun(self: TSNode): TSNode?
---@field next_named_sibling fun(self: TSNode): TSNode?
---@field prev_named_sibling fun(self: TSNode): TSNode?
---@field named_children fun(self: TSNode): TSNode[]
---@field has_changes fun(self: TSNode): boolean
---@field has_error fun(self: TSNode): boolean
---@field sexpr fun(self: TSNode): string
---@field equal fun(self: TSNode, other: TSNode): boolean
---@field iter_children fun(self: TSNode): fun(): TSNode, string
---@field field fun(self: TSNode, name: string): TSNode[]
---@field byte_length fun(self: TSNode): integer
---@field __has_ancestor fun(self: TSNode, node_types: string[]): boolean
local TSNode = {}

---@alias TSLoggerCallback fun(logtype: 'parse'|'lex', msg: string)

---@class TSParser: userdata
---@field parse fun(self: TSParser, tree: TSTree?, source: integer|string, include_bytes: boolean): TSTree, (Range4|Range6)[]
---@field reset fun(self: TSParser)
---@field included_ranges fun(self: TSParser, include_bytes: boolean?): integer[]
---@field set_included_ranges fun(self: TSParser, ranges: (Range6|TSNode)[])
---@field set_timeout fun(self: TSParser, timeout: integer)
---@field timeout fun(self: TSParser): integer
---@field _set_logger fun(self: TSParser, lex: boolean, parse: boolean, cb: TSLoggerCallback)
---@field _logger fun(self: TSParser): TSLoggerCallback

---@class TSTree: userdata
---@field root fun(self: TSTree): TSNode
---@field edit fun(self: TSTree, _: integer, _: integer, _: integer, _: integer, _: integer, _: integer, _: integer, _: integer, _:integer)
---@field copy fun(self: TSTree): TSTree
---@field included_ranges fun(self: TSTree, include_bytes: true): Range6[]
---@field included_ranges fun(self: TSTree, include_bytes: false): Range4[]

---@class TSQuery: userdata
---@field inspect fun(self: TSQuery): TSQueryInfo

---@class (exact) TSQueryInfo
---@field captures string[]
---@field patterns table<integer, (integer|string)[][]>

--- @param lang string
--- @return table
vim._ts_inspect_language = function(lang) end

---@return integer
vim._ts_get_language_version = function() end

--- @param path string
--- @param lang string
--- @param symbol_name? string
vim._ts_add_language_from_object = function(path, lang, symbol_name) end

--- @param path string
--- @param lang string
vim._ts_add_language_from_wasm = function(path, lang) end

---@return integer
vim._ts_get_minimum_language_version = function() end

---@param lang string Language to use for the query
---@param query string Query string in s-expr syntax
---@return TSQuery
vim._ts_parse_query = function(lang, query) end

---@param lang string
---@return TSParser
vim._create_ts_parser = function(lang) end

--- @class TSQueryMatch: userdata
--- @field captures fun(self: TSQueryMatch): table<integer,TSNode[]>
local TSQueryMatch = {}

--- @return integer match_id
--- @return integer pattern_index
function TSQueryMatch:info() end

--- @class TSQueryCursor: userdata
--- @field remove_match fun(self: TSQueryCursor, id: integer)
local TSQueryCursor = {}

--- @return integer capture
--- @return TSNode captured_node
--- @return TSQueryMatch match
function TSQueryCursor:next_capture() end

--- @return TSQueryMatch match
function TSQueryCursor:next_match() end

--- @param node TSNode
--- @param query TSQuery
--- @param start integer?
--- @param stop integer?
--- @param opts? { max_start_depth?: integer, match_limit?: integer}
--- @return TSQueryCursor
function vim._create_ts_querycursor(node, query, start, stop, opts) end
