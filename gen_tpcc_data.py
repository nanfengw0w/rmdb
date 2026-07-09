#!/usr/bin/env python3
import os, random
random.seed(42)
D = "/home/nanfeng/桌面/shujvk/rmdb/src/test/performance_test/table_data_gen"
os.makedirs(D, exist_ok=True)
print("Generating data...")
with open(f"{D}/warehouse.csv","w") as f:
    f.write("w_id,w_name,w_street_1,w_street_2,w_city,w_state,w_zip,w_tax,w_ytd\n")
    f.write("1,W1,s1,s2,city,ST,123456789,0.1,0.0\n")
with open(f"{D}/district.csv","w") as f:
    f.write("d_id,d_w_id,d_name,d_street_1,d_street_2,d_city,d_state,d_zip,d_tax,d_ytd,d_next_o_id\n")
    for d in range(1,11):
        f.write(f"{d},1,D{d},s1,s2,city,ST,123456789,0.1,0.0,1\n")
with open(f"{D}/customer.csv","w") as f:
    f.write("c_id,c_d_id,c_w_id,c_first,c_middle,c_last,c_street_1,c_street_2,c_city,c_state,c_zip,c_phone,c_since,c_credit,c_credit_lim,c_discount,c_balance,c_ytd_payment,c_payment_cnt,c_delivery_cnt,c_data\n")
    for d in range(1,11):
        for c in range(1,3001):
            f.write(f"{c},{d},1,F{c},OE,L{c%20},s1,s2,city,ST,123456789,phone,2026-01-01,GC,50000,0.1,0,0,0,0,data\n")
with open(f"{D}/item.csv","w") as f:
    f.write("i_id,i_im_id,i_name,i_price,i_data\n")
    for i in range(1,100001):
        f.write(f"{i},{i%10000},item{i},{1+i%100}.0,idata\n")
with open(f"{D}/stock.csv","w") as f:
    f.write("s_i_id,s_w_id,s_quantity,s_dist_01,s_dist_02,s_dist_03,s_dist_04,s_dist_05,s_dist_06,s_dist_07,s_dist_08,s_dist_09,s_dist_10,s_ytd,s_order_cnt,s_remote_cnt,s_data\n")
    for i in range(1,100001):
        f.write(f"{i},1,50,d1,d2,d3,d4,d5,d6,d7,d8,d9,d10,0,0,0,sdata\n")
with open(f"{D}/orders.csv","w") as f:
    f.write("o_id,o_d_id,o_w_id,o_c_id,o_entry_d,o_carrier_id,o_ol_cnt,o_all_local\n")
    for d in range(1,11):
        for o in range(1,3001):
            c = ((o-1) % 3000) + 1
            f.write(f"{o},{d},1,{c},2026-01-01,{0 if o>2100 else 1},{5+o%10},1\n")
with open(f"{D}/new_orders.csv","w") as f:
    f.write("no_o_id,no_d_id,no_w_id\n")
    for d in range(1,11):
        for o in range(2101,3001):
            f.write(f"{o},{d},1\n")
with open(f"{D}/order_line.csv","w") as f:
    f.write("ol_o_id,ol_d_id,ol_w_id,ol_number,ol_i_id,ol_supply_w_id,ol_delivery_d,ol_quantity,ol_amount,ol_dist_info\n")
    for d in range(1,11):
        for o in range(1,3001):
            for n in range(1,6):
                iid = ((o*5+n-1) % 100000) + 1
                dl = "2026-01-01" if o <= 2100 else "null"
                f.write(f"{o},{d},1,{n},{iid},1,{dl},5,5.0,dist\n")
with open(f"{D}/history.csv","w") as f:
    f.write("h_c_id,h_c_d_id,h_c_w_id,h_d_id,h_w_id,h_date,h_amount,h_data\n")
    for d in range(1,11):
        for c in range(1,31):
            f.write(f"{c},{d},1,{d},1,2026-01-01,10.0,history\n")
print("Done!")
for f in os.listdir(D):
    lines = sum(1 for _ in open(f"{D}/{f}"))
    print(f"  {f}: {lines} lines")
