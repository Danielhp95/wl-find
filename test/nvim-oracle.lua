-- Each case's first match in nvim, for test/vim-regex.js: { start, end } in bytes,
-- "none", or "error" if nvim refuses the pattern. \v goes first, as in the bar.
local input, output = arg[1], arg[2]
local cases = vim.json.decode(table.concat(vim.fn.readfile(input), "\n"))
local answers = {}
for i, case in ipairs(cases) do
  vim.o.ignorecase = case.ic
  local ok, found = pcall(vim.fn.matchstrpos, case.subject, "\\v" .. case.pattern)
  if not ok then
    answers[i] = "error"
  elseif found[2] < 0 then
    answers[i] = "none"
  else
    answers[i] = { found[2], found[3] }
  end
end
vim.fn.writefile({ vim.json.encode(answers) }, output)
