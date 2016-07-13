// +------------------------------------------------------------------+
// |             ____ _               _        __  __ _  __           |
// |            / ___| |__   ___  ___| | __   |  \/  | |/ /           |
// |           | |   | '_ \ / _ \/ __| |/ /   | |\/| | ' /            |
// |           | |___| | | |  __/ (__|   <    | |  | | . \            |
// |            \____|_| |_|\___|\___|_|\_\___|_|  |_|_|\_\           |
// |                                                                  |
// | Copyright Mathias Kettner 2014             mk@mathias-kettner.de |
// +------------------------------------------------------------------+
//
// This file is part of Check_MK.
// The official homepage is at http://mathias-kettner.de/check_mk.
//
// check_mk is free software;  you can redistribute it and/or modify it
// under the  terms of the  GNU General Public License  as published by
// the Free Software Foundation in version 2.  check_mk is  distributed
// in the hope that it will be useful, but WITHOUT ANY WARRANTY;  with-
// out even the implied warranty of  MERCHANTABILITY  or  FITNESS FOR A
// PARTICULAR PURPOSE. See the  GNU General Public License for more de-
// tails. You should have  received  a copy of the  GNU  General Public
// License along with GNU Make; see the file  COPYING.  If  not,  write
// to the Free Software Foundation, Inc., 51 Franklin St,  Fifth Floor,
// Boston, MA 02110-1301 USA.

#include "Query.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <utility>
#include <vector>
#include "Aggregator.h"
#include "Column.h"
#include "ColumnFilter.h"
#include "Filter.h"
#include "FilterVisitor.h"
#include "Logger.h"
#include "NegatingFilter.h"
#include "NullColumn.h"
#include "OutputBuffer.h"
#include "StatsColumn.h"
#include "Table.h"
#include "VariadicFilter.h"
#include "auth.h"
#include "opids.h"
#include "strutil.h"
#include "waittriggers.h"

extern int g_debug_level;
extern unsigned long g_max_response_size;
extern int g_data_encoding;

using std::list;
using std::runtime_error;
using std::string;
using std::to_string;
using std::unordered_set;
using std::vector;

namespace {
class ColumnCollector : public FilterVisitor {
public:
    explicit ColumnCollector(unordered_set<Column *> &columns)
        : _columns(columns) {}
    void visit(ColumnFilter &f) override { _columns.insert(f.column()); }
    void visit(NegatingFilter &f) override { f.subfilter()->accept(*this); }
    void visit(VariadicFilter &f) override {
        for (const auto &sub_filter : f) {
            sub_filter->accept(*this);
        }
    }

private:
    unordered_set<Column *> &_columns;
};
}  // namespace

Query::Query(const list<string> &lines, Table *table)
    : _renderer(nullptr)
    , _response_header(OutputBuffer::ResponseHeader::off)
    , _do_keepalive(false)
    , _table(table)
    , _filter(this)
    , _auth_user(nullptr)
    , _wait_condition(this)
    , _wait_timeout(0)
    , _wait_trigger(nullptr)
    , _wait_object(nullptr)
    , _field_separator(";")
    , _dataset_separator("\n")
    , _list_separator(",")
    , _host_service_separator("|")
    , _show_column_headers(true)
    , _need_ds_separator(false)
    , _output_format(OutputFormat::csv)
    , _limit(-1)
    , _time_limit(-1)
    , _time_limit_timeout(0)
    , _current_line(0)
    , _timezone_offset(0) {
    for (auto &line : lines) {
        vector<char> line_copy(line.begin(), line.end());
        line_copy.push_back('\0');
        char *buffer = &line_copy[0];
        rstrip(buffer);
        if (g_debug_level > 0) {
            Informational() << "Query: " << buffer;
        }
        if (strncmp(buffer, "Filter:", 7) == 0) {
            parseFilterLine(lstrip(buffer + 7), _filter);

        } else if (strncmp(buffer, "Or:", 3) == 0) {
            parseAndOrLine(lstrip(buffer + 3), LogicalOperator::or_, _filter,
                           "Or");

        } else if (strncmp(buffer, "And:", 4) == 0) {
            parseAndOrLine(lstrip(buffer + 4), LogicalOperator::and_, _filter,
                           "And");

        } else if (strncmp(buffer, "Negate:", 7) == 0) {
            parseNegateLine(lstrip(buffer + 7), _filter, "Negate");

        } else if (strncmp(buffer, "StatsOr:", 8) == 0) {
            parseStatsAndOrLine(lstrip(buffer + 8), LogicalOperator::or_);

        } else if (strncmp(buffer, "StatsAnd:", 9) == 0) {
            parseStatsAndOrLine(lstrip(buffer + 9), LogicalOperator::and_);

        } else if (strncmp(buffer, "StatsNegate:", 12) == 0) {
            parseStatsNegateLine(lstrip(buffer + 12));

        } else if (strncmp(buffer, "Stats:", 6) == 0) {
            parseStatsLine(lstrip(buffer + 6));

        } else if (strncmp(buffer, "StatsGroupBy:", 13) == 0) {
            parseStatsGroupLine(lstrip(buffer + 13));

        } else if (strncmp(buffer, "Columns:", 8) == 0) {
            parseColumnsLine(lstrip(buffer + 8));

        } else if (strncmp(buffer, "ColumnHeaders:", 14) == 0) {
            parseColumnHeadersLine(lstrip(buffer + 14));

        } else if (strncmp(buffer, "Limit:", 6) == 0) {
            parseLimitLine(lstrip(buffer + 6));

        } else if (strncmp(buffer, "Timelimit:", 10) == 0) {
            parseTimelimitLine(lstrip(buffer + 10));

        } else if (strncmp(buffer, "AuthUser:", 9) == 0) {
            parseAuthUserHeader(lstrip(buffer + 9));

        } else if (strncmp(buffer, "Separators:", 11) == 0) {
            parseSeparatorsLine(lstrip(buffer + 11));

        } else if (strncmp(buffer, "OutputFormat:", 13) == 0) {
            parseOutputFormatLine(lstrip(buffer + 13));

        } else if (strncmp(buffer, "ResponseHeader:", 15) == 0) {
            parseResponseHeaderLine(lstrip(buffer + 15));

        } else if (strncmp(buffer, "KeepAlive:", 10) == 0) {
            parseKeepAliveLine(lstrip(buffer + 10));

        } else if (strncmp(buffer, "WaitCondition:", 14) == 0) {
            parseFilterLine(lstrip(buffer + 14), _wait_condition);

        } else if (strncmp(buffer, "WaitConditionAnd:", 17) == 0) {
            parseAndOrLine(lstrip(buffer + 17), LogicalOperator::and_,
                           _wait_condition, "WaitConditionAnd");

        } else if (strncmp(buffer, "WaitConditionOr:", 16) == 0) {
            parseAndOrLine(lstrip(buffer + 16), LogicalOperator::or_,
                           _wait_condition, "WaitConditionOr");

        } else if (strncmp(buffer, "WaitConditionNegate:", 20) == 0) {
            parseNegateLine(lstrip(buffer + 20), _wait_condition,
                            "WaitConditionNegate");

        } else if (strncmp(buffer, "WaitTrigger:", 12) == 0) {
            parseWaitTriggerLine(lstrip(buffer + 12));

        } else if (strncmp(buffer, "WaitObject:", 11) == 0) {
            parseWaitObjectLine(lstrip(buffer + 11));

        } else if (strncmp(buffer, "WaitTimeout:", 12) == 0) {
            parseWaitTimeoutLine(lstrip(buffer + 12));

        } else if (strncmp(buffer, "Localtime:", 10) == 0) {
            parseLocaltimeLine(lstrip(buffer + 10));

        } else if (buffer[0] == 0) {
            break;

        } else {
            invalidHeader("Undefined request header '" + string(buffer) + "'");
            break;
        }
    }

    if (_columns.empty() && !doStats()) {
        table->any_column([this](Column *c) { return addColumn(c), false; });
        // TODO(sp) We overwrite the value from a possible ColumnHeaders: line
        // here, is that really what we want?
        _show_column_headers = true;
    }

    _all_columns.insert(_columns.begin(), _columns.end());
    for (const auto &sc : _stats_columns) {
        _all_columns.insert(sc->column());
    }

    ColumnCollector cc(_all_columns);
    _filter.accept(cc);
    _wait_condition.accept(cc);
}

Query::~Query() {
    // delete dynamic columns
    for (auto column : _columns) {
        if (column->mustDelete()) {
            delete column;
        }
    }

    for (auto &dummy_column : _dummy_columns) {
        delete dummy_column;
    }
    for (auto &stats_column : _stats_columns) {
        delete stats_column;
    }
}

Column *Query::createDummyColumn(const char *name) {
    Column *col = new NullColumn(name, "Non existing column");
    _dummy_columns.push_back(col);
    return col;
}

void Query::addColumn(Column *column) { _columns.push_back(column); }

size_t Query::size() { return _renderer->size(); }

void Query::add(const string &str) { _renderer->add(str); }

void Query::add(const vector<char> &blob) { _renderer->add(blob); }

void Query::setResponseHeader(OutputBuffer::ResponseHeader r) {
    _response_header = r;
}

void Query::setDoKeepalive(bool d) { _do_keepalive = d; }

void Query::invalidHeader(const string &message) {
    if (_invalid_header_message == "") {
        _invalid_header_message = message;
    }
}

void Query::invalidRequest(const string &message) {
    _renderer->setError(OutputBuffer::ResponseCode::invalid_request, message);
}

void Query::limitExceeded(const string &message) {
    _renderer->setError(OutputBuffer::ResponseCode::limit_exceeded, message);
}

Filter *Query::createFilter(Column *column, RelationalOperator relOp,
                            const string &value) {
    try {
        return column->createFilter(this, relOp, value);
    } catch (const runtime_error &e) {
        invalidHeader("error creating filter on table" +
                      string(_table->name()) + ": " + e.what());
        return nullptr;
    }
}

void Query::parseAndOrLine(char *line, LogicalOperator andor,
                           VariadicFilter &filter, string header) {
    char *value = next_field(&line);
    if (value == nullptr) {
        invalidHeader("Missing value for " + header +
                      ": need positive integer number");
        return;
    }

    int number = atoi(value);
    if (isdigit(value[0]) == 0 || number <= 0) {
        invalidHeader("Invalid value for " + header +
                      ": need positive integer number");
        return;
    }

    filter.combineFilters(this, number, andor);
}

void Query::parseNegateLine(char *line, VariadicFilter &filter, string header) {
    if (next_field(&line) != nullptr) {
        invalidHeader(header + ": does not take any arguments");
        return;
    }

    Filter *to_negate = filter.stealLastSubfiler();
    if (to_negate == nullptr) {
        invalidHeader(header + " nothing to negate");
        return;
    }

    filter.addSubfilter(new NegatingFilter(this, to_negate));
}

void Query::parseStatsAndOrLine(char *line, LogicalOperator andor) {
    string kind = andor == LogicalOperator::or_ ? "StatsOr" : "StatsAnd";
    char *value = next_field(&line);
    if (value == nullptr) {
        invalidHeader("Missing value for " + kind +
                      ": need non-zero integer number");
        return;
    }

    int number = atoi(value);
    if ((isdigit(value[0]) == 0) || number <= 0) {
        invalidHeader("Invalid value for " + kind +
                      " : need non-zero integer number");
        return;
    }

    // The last 'number' StatsColumns must be of type StatsOperation::count
    auto variadic = VariadicFilter::make(this, andor);
    while (number > 0) {
        if (_stats_columns.empty()) {
            invalidHeader("Invalid count for " + kind +
                          ": too few Stats: headers available");
            return;
        }

        StatsColumn *col = _stats_columns.back();
        if (col->operation() != StatsOperation::count) {
            invalidHeader("Can use " + kind +
                          " only on Stats: headers of filter type");
            return;
        }
        variadic->addSubfilter(col->stealFilter());
        delete col;
        _stats_columns.pop_back();
        number--;
    }
    // TODO(sp) Use unique_ptr in StatsColumn.
    _stats_columns.push_back(
        new StatsColumn(nullptr, variadic.release(), StatsOperation::count));
}

void Query::parseStatsNegateLine(char *line) {
    if (next_field(&line) != nullptr) {
        invalidHeader("StatsNegate: does not take any arguments");
        return;
    }
    if (_stats_columns.empty()) {
        invalidHeader("StatsNegate: no Stats: headers available");
        return;
    }
    StatsColumn *col = _stats_columns.back();
    if (col->operation() != StatsOperation::count) {
        invalidHeader(
            "Can use StatsNegate only on Stats: headers of filter type");
        return;
    }
    auto negated = new NegatingFilter(this, col->stealFilter());
    delete col;
    _stats_columns.pop_back();
    _stats_columns.push_back(
        new StatsColumn(nullptr, negated, StatsOperation::count));
}

void Query::parseStatsLine(char *line) {
    // first token is either aggregation operator or column name
    char *col_or_op = next_field(&line);
    if (col_or_op == nullptr) {
        invalidHeader("empty stats line");
        return;
    }

    StatsOperation operation = StatsOperation::count;
    if (strcmp(col_or_op, "sum") == 0) {
        operation = StatsOperation::sum;
    } else if (strcmp(col_or_op, "min") == 0) {
        operation = StatsOperation::min;
    } else if (strcmp(col_or_op, "max") == 0) {
        operation = StatsOperation::max;
    } else if (strcmp(col_or_op, "avg") == 0) {
        operation = StatsOperation::avg;
    } else if (strcmp(col_or_op, "std") == 0) {
        operation = StatsOperation::std;
    } else if (strcmp(col_or_op, "suminv") == 0) {
        operation = StatsOperation::suminv;
    } else if (strcmp(col_or_op, "avginv") == 0) {
        operation = StatsOperation::avginv;
    }

    char *column_name;
    if (operation == StatsOperation::count) {
        column_name = col_or_op;
    } else {
        // aggregation operator is followed by column name
        column_name = next_field(&line);
        if (column_name == nullptr) {
            invalidHeader("missing column name in stats header");
            return;
        }
    }

    Column *column = _table->column(column_name);
    if (column == nullptr) {
        invalidHeader("invalid stats header: table '" + string(_table->name()) +
                      "' has no column '" + string(column_name) + "'");
        return;
    }

    StatsColumn *stats_col;
    if (operation == StatsOperation::count) {
        char *operator_name = next_field(&line);
        if (operator_name == nullptr) {
            invalidHeader(
                "invalid stats header: missing operator after table '" +
                string(column_name) + "'");
            return;
        }
        RelationalOperator relOp;
        if (!relationalOperatorForName(operator_name, relOp)) {
            invalidHeader("invalid stats operator '" + string(operator_name) +
                          "'");
            return;
        }
        char *value = lstrip(line);
        if (value == nullptr) {
            invalidHeader("invalid stats: missing value after operator '" +
                          string(operator_name) + "'");
            return;
        }

        Filter *filter = createFilter(column, relOp, value);
        if (filter == nullptr) {
            return;
        }
        stats_col = new StatsColumn(column, filter, operation);
    } else {
        stats_col = new StatsColumn(column, nullptr, operation);
    }
    _stats_columns.push_back(stats_col);

    /* Default to old behaviour: do not output column headers if we
       do Stats queries */
    _show_column_headers = false;
}

void Query::parseFilterLine(char *line, VariadicFilter &filter) {
    char *column_name = next_field(&line);
    if (column_name == nullptr) {
        invalidHeader("empty filter line");
        return;
    }

    Column *column = _table->column(column_name);
    if (column == nullptr) {
        invalidHeader("invalid filter: table '" + string(_table->name()) +
                      "' has no column '" + string(column_name) + "'");
        return;
    }

    char *operator_name = next_field(&line);
    if (operator_name == nullptr) {
        invalidHeader("invalid filter header: missing operator after table '" +
                      string(column_name) + "'");
        return;
    }
    RelationalOperator relOp;
    if (!relationalOperatorForName(operator_name, relOp)) {
        invalidHeader("invalid filter operator '" + string(operator_name) +
                      "'");
        return;
    }
    char *value = lstrip(line);
    if (value == nullptr) {
        invalidHeader("invalid filter: missing value after operator '" +
                      string(operator_name) + "'");
        return;
    }

    if (Filter *sub_filter = createFilter(column, relOp, value)) {
        filter.addSubfilter(sub_filter);
    }
}

void Query::parseAuthUserHeader(char *line) {
    _auth_user = find_contact(line);
    if (_auth_user == nullptr) {
        // Do not handle this as error any more. In a multi site setup
        // not all users might be present on all sites by design.
        _auth_user = UNKNOWN_AUTH_USER;
    }
}

void Query::parseStatsGroupLine(char *line) {
    Warning()
        << "Warning: StatsGroupBy is deprecated. Please use Columns instead.";
    parseColumnsLine(line);
}

void Query::parseColumnsLine(char *line) {
    char *column_name;
    while (nullptr != (column_name = next_field(&line))) {
        Column *column = _table->column(column_name);
        if (column != nullptr) {
            _columns.push_back(column);
        } else {
            Warning() << "Replacing non-existing column '"
                      << string(column_name) << "' with null column";
            // Do not fail any longer. We might want to make this configurable.
            // But not failing has the advantage that an updated GUI, that
            // expects new columns,
            // will be able to keep compatibility with older Livestatus
            // versions.
            // invalidHeader(
            //       "Table '%s' has no column '%s'", _table->name(),
            //       column_name);
            Column *col = createDummyColumn(column_name);
            _columns.push_back(col);
        }
    }
    _show_column_headers = false;
}

void Query::parseSeparatorsLine(char *line) {
    char dssep = 0, fieldsep = 0, listsep = 0, hssep = 0;
    char *token = next_field(&line);
    if (token != nullptr) {
        dssep = atoi(token);
    }
    token = next_field(&line);
    if (token != nullptr) {
        fieldsep = atoi(token);
    }
    token = next_field(&line);
    if (token != nullptr) {
        listsep = atoi(token);
    }
    token = next_field(&line);
    if (token != nullptr) {
        hssep = atoi(token);
    }

    _dataset_separator = string(&dssep, 1);
    _field_separator = string(&fieldsep, 1);
    _list_separator = string(&listsep, 1);
    _host_service_separator = string(&hssep, 1);
}

void Query::parseOutputFormatLine(char *line) {
    char *format = next_field(&line);
    if (format == nullptr) {
        invalidHeader(
            "Missing output format. Only 'csv' and 'json' are available.");
        return;
    }

    if (strcmp(format, "csv") == 0) {
        _output_format = OutputFormat::csv;
    } else if (strcmp(format, "json") == 0) {
        _output_format = OutputFormat::json;
    } else if (strcmp(format, "python") == 0) {
        _output_format = OutputFormat::python;
    } else {
        invalidHeader(
            "Invalid output format. Only 'csv' and 'json' are available.");
    }
}

void Query::parseColumnHeadersLine(char *line) {
    char *value = next_field(&line);
    if (value == nullptr) {
        invalidHeader("Missing value for ColumnHeaders: must be 'on' or 'off'");
        return;
    }

    if (strcmp(value, "on") == 0) {
        _show_column_headers = true;
    } else if (strcmp(value, "off") == 0) {
        _show_column_headers = false;
    } else {
        invalidHeader("Invalid value for ColumnHeaders: must be 'on' or 'off'");
    }
}

void Query::parseKeepAliveLine(char *line) {
    char *value = next_field(&line);
    if (value == nullptr) {
        invalidHeader("Missing value for KeepAlive: must be 'on' or 'off'");
        return;
    }

    if (strcmp(value, "on") == 0) {
        setDoKeepalive(true);
    } else if (strcmp(value, "off") == 0) {
        setDoKeepalive(false);
    } else {
        invalidHeader("Invalid value for KeepAlive: must be 'on' or 'off'");
    }
}

void Query::parseResponseHeaderLine(char *line) {
    char *value = next_field(&line);
    if (value == nullptr) {
        invalidHeader(
            "Missing value for ResponseHeader: must be 'off' or 'fixed16'");
        return;
    }

    if (strcmp(value, "off") == 0) {
        setResponseHeader(OutputBuffer::ResponseHeader::off);
    } else if (strcmp(value, "fixed16") == 0) {
        setResponseHeader(OutputBuffer::ResponseHeader::fixed16);
    } else {
        invalidHeader("Invalid value '" + string(value) +
                      "' for ResponseHeader: must be 'off' or 'fixed16'");
    }
}

void Query::parseLimitLine(char *line) {
    char *value = next_field(&line);
    if (value == nullptr) {
        invalidHeader("Header Limit: missing value");
    } else {
        int limit = atoi(value);
        if ((isdigit(value[0]) == 0) || limit < 0) {
            invalidHeader(
                "Invalid value for Limit: must be non-negative integer");
        } else {
            _limit = limit;
        }
    }
}

void Query::parseTimelimitLine(char *line) {
    char *value = next_field(&line);
    if (value == nullptr) {
        invalidHeader("Header Timelimit: missing value");
    } else {
        int timelimit = atoi(value);
        if ((isdigit(value[0]) == 0) || timelimit < 0) {
            invalidHeader(
                "Invalid value for Timelimit: must be "
                "non-negative integer (seconds)");
        } else {
            _time_limit = timelimit;
            _time_limit_timeout = time(nullptr) + _time_limit;
        }
    }
}

void Query::parseWaitTimeoutLine(char *line) {
    char *value = next_field(&line);
    if (value == nullptr) {
        invalidHeader("WaitTimeout: missing value");
    } else {
        int timeout = atoi(value);
        if ((isdigit(value[0]) == 0) || timeout < 0) {
            invalidHeader(
                "Invalid value for WaitTimeout: must be non-negative integer");
        } else {
            _wait_timeout = timeout;
        }
    }
}

void Query::parseWaitTriggerLine(char *line) {
    char *value = next_field(&line);
    if (value == nullptr) {
        invalidHeader("WaitTrigger: missing keyword");
        return;
    }
    struct trigger *t = trigger_find(value);
    if (t == nullptr) {
        invalidHeader("WaitTrigger: invalid trigger '" + string(value) +
                      "'. Allowed are " + trigger_all_names() + ".");
        return;
    }
    _wait_trigger = t;
}

void Query::parseWaitObjectLine(char *line) {
    char *objectspec = lstrip(line);
    _wait_object = _table->findObject(objectspec);
    if (_wait_object == nullptr) {
        invalidHeader("WaitObject: object '" + string(objectspec) +
                      "' not found or not supported by this table");
    }
}

void Query::parseLocaltimeLine(char *line) {
    char *value = next_field(&line);
    if (value == nullptr) {
        invalidHeader("Header Localtime: missing value");
        return;
    }
    time_t their_time = atoi(value);
    time_t our_time = time(nullptr);

    // compute offset to be *added* each time we output our time and
    // *substracted* from reference value by filter headers
    int dif = their_time - our_time;

    // Round difference to half hour. We assume, that both clocks are more
    // or less synchronized and that the time offset is only due to being
    // in different time zones.
    int full = dif / 1800;
    int rem = dif % 1800;
    if (rem <= -900) {
        full--;
    } else if (rem >= 900) {
        full++;
    }
    if (full >= 48 || full <= -48) {
        invalidHeader(
            "Invalid Localtime header: timezone difference "
            "greater then 24 hours");
        return;
    }
    _timezone_offset = full * 1800;
    if (g_debug_level >= 2) {
        Informational() << "Timezone difference is "
                        << (_timezone_offset / 3600.0) << " hours";
    }
}

bool Query::doStats() { return !_stats_columns.empty(); }

void Query::process(OutputBuffer *output) {
    Renderer renderer(output, _response_header, _do_keepalive,
                      _invalid_header_message, _output_format, _field_separator,
                      _dataset_separator, _list_separator,
                      _host_service_separator, _timezone_offset);
    _renderer = &renderer;
    start();
    _table->answerQuery(this);
    finish();
}

void Query::start() {
    doWait();

    _need_ds_separator = false;

    _renderer->startOfQuery();

    if (doStats()) {
        // if we have no StatsGroupBy: column, we allocate one only row of
        // Aggregators,
        // directly in _stats_aggregators. When grouping the rows of aggregators
        // will be created each time a new group is found.
        if (_columns.empty()) {
            _stats_aggregators = new Aggregator *[_stats_columns.size()];
            for (unsigned i = 0; i < _stats_columns.size(); i++) {
                _stats_aggregators[i] = _stats_columns[i]->createAggregator();
            }
        }
    }

    if (_show_column_headers) {
        outputDatasetBegin();
        bool first = true;

        for (const auto &column : _columns) {
            if (first) {
                first = false;
            } else {
                outputFieldSeparator();
            }
            outputString(column->name());
        }

        // Output dummy headers for stats columns
        int col = 1;
        for (const auto &stats_column : _stats_columns) {
            (void)stats_column;
            if (first) {
                first = false;
            } else {
                outputFieldSeparator();
            }
            char colheader[32];
            snprintf(colheader, 32, "stats_%d", col);
            outputString(colheader);
            col++;
        }

        outputDatasetEnd();
        _need_ds_separator = true;
    }
}

bool Query::timelimitReached() {
    if (_time_limit >= 0 && time(nullptr) >= _time_limit_timeout) {
        Informational() << "Maximum query time of " << _time_limit
                        << " seconds exceeded!";
        limitExceeded("Maximum query time of " + to_string(_time_limit) +
                      " seconds exceeded!");
        return true;
    }
    return false;
}

bool Query::processDataset(void *data) {
    if (size() > g_max_response_size) {
        Informational() << "Maximum response size of " << g_max_response_size
                        << " bytes exceeded!";
        // currently we only log an error into the log file and do
        // not abort the query. We handle it like Limit:
        return false;
    }

    if (_filter.accepts(data) &&
        ((_auth_user == nullptr) || _table->isAuthorized(_auth_user, data))) {
        _current_line++;
        if (_limit >= 0 && static_cast<int>(_current_line) > _limit) {
            return false;
        }

        // When we reach the time limit we let the query fail. Otherwise the
        // user will not know that the answer is incomplete.
        if (timelimitReached()) {
            return false;
        }

        if (doStats()) {
            Aggregator **aggr;
            // When doing grouped stats, we need to fetch/create a row of
            // aggregators for the current group
            if (!_columns.empty()) {
                _stats_group_spec_t groupspec;
                computeStatsGroupSpec(groupspec, data);
                aggr = getStatsGroup(groupspec);
            } else {
                aggr = _stats_aggregators;
            }

            for (unsigned i = 0; i < _stats_columns.size(); i++) {
                aggr[i]->consume(data, this);
            }

            // No output is done while processing the data, we only collect
            // stats.
        } else {
            // output data of current row
            if (_need_ds_separator) {
                _renderer->outputDataSetSeparator();
            } else {
                _need_ds_separator = true;
            }

            outputDatasetBegin();
            bool first = true;
            for (auto column : _columns) {
                if (first) {
                    first = false;
                } else {
                    outputFieldSeparator();
                }
                column->output(data, this);
            }
            outputDatasetEnd();
        }
    }
    return true;
}

void Query::finish() {
    // grouped stats
    if (doStats() && !_columns.empty()) {
        // output values of all stats groups (output has been post poned until
        // now)
        for (auto &stats_group : _stats_groups) {
            if (_need_ds_separator) {
                _renderer->outputDataSetSeparator();
            } else {
                _need_ds_separator = true;
            }

            outputDatasetBegin();

            // output group columns first
            _stats_group_spec_t groupspec = stats_group.first;
            bool first = true;
            for (auto &iit : groupspec) {
                if (!first) {
                    outputFieldSeparator();
                } else {
                    first = false;
                }
                outputString(iit.c_str());
            }

            Aggregator **aggr = stats_group.second;
            for (unsigned i = 0; i < _stats_columns.size(); i++) {
                outputFieldSeparator();
                aggr[i]->output(_renderer);
                delete aggr[i];  // not needed any more
            }
            outputDatasetEnd();
            delete[] aggr;
        }
    }

    // stats without group column
    else if (doStats()) {
        if (_need_ds_separator) {
            _renderer->outputDataSetSeparator();
        } else {
            _need_ds_separator = true;
        }

        outputDatasetBegin();
        for (unsigned i = 0; i < _stats_columns.size(); i++) {
            if (i > 0) {
                outputFieldSeparator();
            }
            _stats_aggregators[i]->output(_renderer);
            delete _stats_aggregators[i];
        }
        outputDatasetEnd();
        delete[] _stats_aggregators;
    }

    // normal query
    _renderer->endOfQuery();
}

const string *Query::findValueForIndexing(const string &column_name) {
    return _filter.findValueForIndexing(column_name);
}

void Query::findIntLimits(const string &column_name, int *lower, int *upper) {
    return _filter.findIntLimits(column_name, lower, upper);
}

void Query::optimizeBitmask(const string &column_name, uint32_t *bitmask) {
    _filter.optimizeBitmask(column_name, bitmask);
}

// output helpers, called from columns
void Query::outputDatasetBegin() { _renderer->outputDatasetBegin(); }

void Query::outputDatasetEnd() { _renderer->outputDatasetEnd(); }

void Query::outputFieldSeparator() { _renderer->outputFieldSeparator(); }

void Query::outputInteger(int32_t value) { _renderer->outputInteger(value); }

void Query::outputInteger64(int64_t value) {
    _renderer->outputInteger64(value);
}

void Query::outputTime(int32_t value) { _renderer->outputTime(value); }

void Query::outputUnsignedLong(unsigned long value) {
    _renderer->outputUnsignedLong(value);
}

void Query::outputCounter(counter_t value) { _renderer->outputCounter(value); }

void Query::outputDouble(double value) { _renderer->outputDouble(value); }

void Query::outputNull() { _renderer->outputNull(); }

void Query::outputAsciiEscape(char value) {
    _renderer->outputAsciiEscape(value);
}

void Query::outputUnicodeEscape(unsigned value) {
    _renderer->outputUnicodeEscape(value);
}

void Query::outputBlob(const vector<char> *blob) {
    _renderer->outputBlob(blob);
}

void Query::outputString(const char *value, int len) {
    _renderer->outputString(value, len);
}

void Query::outputBeginList() { _renderer->outputBeginList(); }

void Query::outputListSeparator() { _renderer->outputListSeparator(); }

void Query::outputEndList() { _renderer->outputEndList(); }

void Query::outputBeginSublist() { _renderer->outputBeginSublist(); }

void Query::outputSublistSeparator() { _renderer->outputSublistSeparator(); }

void Query::outputEndSublist() { _renderer->outputEndSublist(); }

void Query::outputBeginDict() { _renderer->outputBeginDict(); }

void Query::outputDictSeparator() { _renderer->outputDictSeparator(); }

void Query::outputDictValueSeparator() {
    _renderer->outputDictValueSeparator();
}

void Query::outputEndDict() { _renderer->outputEndDict(); }

Aggregator **Query::getStatsGroup(Query::_stats_group_spec_t &groupspec) {
    auto it = _stats_groups.find(groupspec);
    if (it == _stats_groups.end()) {
        auto aggr = new Aggregator *[_stats_columns.size()];
        for (unsigned i = 0; i < _stats_columns.size(); i++) {
            aggr[i] = _stats_columns[i]->createAggregator();
        }
        _stats_groups.insert(make_pair(groupspec, aggr));
        return aggr;
    }
    return it->second;
}

void Query::computeStatsGroupSpec(Query::_stats_group_spec_t &groupspec,
                                  void *data) {
    for (auto column : _columns) {
        groupspec.push_back(column->valueAsString(data, this));
    }
}

void Query::doWait() {
    // If no wait condition and no trigger is set,
    // we do not wait at all.
    if (!_wait_condition.hasSubFilters() && _wait_trigger == nullptr) {
        return;
    }

    // If a condition is set, we check the condition. If it
    // is already true, we do not need to way
    if (_wait_condition.hasSubFilters() &&
        _wait_condition.accepts(_wait_object)) {
        if (g_debug_level >= 2) {
            Informational() << "Wait condition true, no waiting neccessary";
        }
        return;
    }

    // No wait on specified trigger. If no trigger was specified
    // we use WT_ALL as default trigger.
    if (_wait_trigger == nullptr) {
        _wait_trigger = trigger_all();
    }

    do {
        if (_wait_timeout == 0) {
            if (g_debug_level >= 2) {
                Informational()
                    << "Waiting unlimited until condition becomes true";
            }
            trigger_wait(_wait_trigger);
        } else {
            if (g_debug_level >= 2) {
                Informational() << "Waiting " << _wait_timeout
                                << "ms or until condition becomes true";
            }
            if (trigger_wait_for(_wait_trigger, _wait_timeout) == 0) {
                if (g_debug_level >= 2) {
                    Informational() << "WaitTimeout after " << _wait_timeout
                                    << "ms";
                }
                return;  // timeout occurred. do not wait any longer
            }
        }
    } while (!_wait_condition.accepts(_wait_object));
}
