# -*- coding: utf-8 -*-
import re

with open(r'D:\HanjiaXiangmu\shujvk\rmdb\src\rmdb.cpp', 'r', encoding='utf-8') as f:
    lines = f.readlines()

# Find the line with "outfile << "failure\n";" after the RMDBError catch
in_catch_block = False
insert_line = -1
for i, line in enumerate(lines):
    if 'catch (RMDBError &e)' in line:
        in_catch_block = True
    if in_catch_block and '                }' == line.rstrip():
        insert_line = i
        break

if insert_line == -1:
    print("Could not find insertion point")
else:
    new_lines = [
        '                } catch (std::exception &e) {\n',
        '                    std::cerr << "Standard exception: " << e.what() << std::endl;\n',
        '                    std::string err_msg = std::string("Error: ") + e.what();\n',
        '                    memcpy(data_send, err_msg.c_str(), err_msg.length());\n',
        '                    data_send[err_msg.length()] = \'\\n\';\n',
        '                    offset = err_msg.length();\n',
        '                    std::fstream outfile;\n',
        '                    outfile.open("output.txt", std::ios::out | std::ios::app);\n',
        '                    outfile << "failure\\n";\n',
        '                    outfile.close();\n',
        '                } catch (...) {\n',
        '                    std::cerr << "Unknown exception caught" << std::endl;\n',
        '                    std::string err_msg = "Error: Unknown error";\n',
        '                    memcpy(data_send, err_msg.c_str(), err_msg.length());\n',
        '                    data_send[err_msg.length()] = \'\\n\';\n',
        '                    offset = err_msg.length();\n',
        '                    std::fstream outfile;\n',
        '                    outfile.open("output.txt", std::ios::out | std::ios::app);\n',
        '                    outfile << "failure\\n";\n',
        '                    outfile.close();\n',
    ]
    lines[insert_line:insert_line] = new_lines
    with open(r'D:\HanjiaXiangmu\shujvk\rmdb\src\rmdb.cpp', 'w', encoding='utf-8') as f:
        f.writelines(lines)
    print("Successfully added catch blocks at line", insert_line)
