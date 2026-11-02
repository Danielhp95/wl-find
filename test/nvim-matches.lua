-- Every match of each case's pattern in its subject, for test/presets.js: a list
-- of the matched texts, or "error" if nvim refuses the pattern. Case goes as in
-- the bar, with 'ignorecase' and 'smartcase'.
local input, output = arg[1], arg[2]
local cases = vim.json.decode(table.concat(vim.fn.readfile(input), "\n"))
vim.o.ignorecase = true
vim.o.smartcase = true
local answers = {}
for i, case in ipairs(cases) do
  local ok, found = pcall(vim.fn.matchstrlist, { case.subject }, case.pattern)
  if not ok then
    answers[i] = "error"
  else
    local texts = {}
    for _, match in ipairs(found) do
      if #match.text > 0 then table.insert(texts, match.text) end
    end
    -- An empty list would encode as {}, and a JSON object is not what is wanted.
    answers[i] = #texts > 0 and texts or vim.empty_dict()
  end
end
vim.fn.writefile({ vim.json.encode(answers) }, output)
