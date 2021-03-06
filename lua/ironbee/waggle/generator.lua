#!/usr/bin/lua

--[[--------------------------------------------------------------------------
-- Licensed to Qualys, Inc. (QUALYS) under one or more
-- contributor license agreements.  See the NOTICE file distributed with
-- this work for additional information regarding copyright ownership.
-- QUALYS licenses this file to You under the Apache License, Version 2.0
-- (the "License"); you may not use this file except in compliance with
-- the License.  You may obtain a copy of the License at
--
--     http://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.
--]]--------------------------------------------------------------------------

--
-- IronBee Waggle --- Generator
--
-- Generates rules.
--
-- @author Sam Baskinger <sbaskinger@qualys.com>
local Util = require('ironbee/waggle/util')
local Action = require('ironbee/waggle/actionrule')
local RuleExt = require('ironbee/waggle/ruleext')
local StreamInspect = require('ironbee/waggle/streaminspect')

-- ###########################################################################
-- Generator - generate a rules.conf or similar
-- ###########################################################################
local Generator = {}
Generator.__index = Generator
Generator.type = 'generator'
Generator.new = function(self)
    local g = {}
    return setmetatable(g, self)
end

Generator.gen_op = function(self, rule)
    if rule.is_a(RuleExt) then
        return rule.data.op
    else
        if string.sub(rule.data.op, 1, 1) == '!' then
            return string.format("!@%s", string.sub(rule.data.op, 2))
        else
            return string.format("@%s", rule.data.op)
        end
    end
end

Generator.gen_fields = function(self, rule)
    local field_list = {}

    for _, field in ipairs(rule.data.fields) do
        local f = field.collection

        if field.selector then
            f = f .. ':' .. field.selector
        end

        if field.transformation then
            f = f .. '.' .. field.transformation
        end

        table.insert(field_list, f)
    end

    return '"' .. table.concat(field_list, '" "') .. '"'
end

-- Generate actions, including tags, etc.
--
-- This ignores the 'chain' action. That is inserted as-needed.
Generator.gen_actions = function(self, rule)
    local t = {}

    -- Add the tags.
    for val,_ in pairs(rule.data.tags) do
        table.insert(t, "tag:"..val)
    end

    -- Add the actions that are not id, rev, or phase.
    -- They may only appear once.
    for _, act in pairs(rule.data.actions) do
        if  act.name ~= 'id'
        and act.name ~= 'rev'
        and act.name ~= 'phase'
        then
            if act.argument then
                table.insert(t, act.name .. ":"..act.argument)
            else
                table.insert(t, act.name)
            end
        end
    end

    -- Add the message if it exists.
    if rule.data.message then
        table.insert(t, "msg:"..rule.data.message)
    end

    if #t > 0 then
        return '"' .. table.concat(t, '" "') .. '"'
    else
        return ''
    end
end

-- Generate a string that represents an IronBee rules configuration.
--
-- @param[in] self The generator.
-- @param[in] plan The plan generated by Planner:new():plan(db) or equivalent.
-- @param[in] db The SignatureDatabase used to plan.
Generator.generate = function(self, plan, db)
    -- String we are going to return representing the final configuration.
    local s = ''

    for _, chain in ipairs(plan) do

        for i, link in ipairs(chain) do
            local rule_id = link.rule
            local result = link.result
            local rule = db.db[rule_id]

            if rule:is_a(Rule) then
                if rule.data.comment then
                    s = s .. "# ".. string.gsub(rule.data.comment, "\n", "\n# ") .."\n"
                end
                s = s .. string.format(
                    "%s %s %s \"%s\" %s",
                    rule.data.rule_type,
                    self:gen_fields(rule),
                    self:gen_op(rule),
                    rule.data.op_arg,
                    self:gen_actions(rule))

                -- The first rule in a chain gets the ID, message, phase, etc.
                if i == 1 then
                    local last_rule = db.db[chain[#chain].rule]
                    s = s .. ' "id:' .. last_rule.data.id .. '"'
                    s = s .. ' "rev:' ..  last_rule.data.version .. '"'
                    if last_rule.data.phase then
                        s = s .. ' "phase:' ..  last_rule.data.phase .. '"'
                    end
                end

                if i ~= #chain then
                    s = s .. ' chain'
                end
            elseif rule:is_a(RuleExt) then
                if rule.data.comment then
                    s = s .. "# ".. string.gsub(rule.data.comment, "\n", "\n# ") .."\n"
                end
                s = s .. string.format(
                    "%s %s %s \"%s\" %s \"id:%s\" \"rev:%s\"",
                    rule.data.rule_type,
                    self:gen_fields(rule),
                    self:gen_op(rule),
                    rule.data.op_arg,
                    self:gen_actions(rule),
                    rule.data.id,
                    rule.data.version)

            elseif rule:is_a(Action) then
                if rule.data.comment then
                    s = s .. "# ".. string.gsub(rule.data.comment, "\n", "\n# ") .."\n"
                end
                s = s .. string.format(
                    "%s %s \"id:%s\" \"rev:%s\"",
                    rule.data.rule_type,
                    self:gen_actions(rule),
                    rule.data.id,
                    rule.data.version)
            elseif rule.is_a(StreamInspect) then
                if rule.data.comment then
                    s = s .. "# ".. string.gsub(rule.data.comment, "\n", "\n# ") .."\n"
                end
                s = s .. string.format(
                    "%s %s %s \"%s\" %s \"id:%s\" \"rev:%s\"",
                    rule.data.rule_type,
                    self:gen_fields(rule),
                    self:gen_op(rule),
                    rule.data.op_arg,
                    self:gen_actions(rule),
                    rule.data.id,
                    rule.data.version)
            end

            s = s .. "\n"
        end
    end

    return s
end

return Generator
