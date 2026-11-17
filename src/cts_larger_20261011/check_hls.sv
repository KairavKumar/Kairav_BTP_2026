module check_hls;
  reg clk=0;
  always #5 clk=~clk;
  reg [3:0] a=0,b=0;
  reg [22:0] r=0;
  wire [3:0] y0,y1;
  sbox dut(.clk(clk),
    .dec_1(1'b0),
    .x0_0(a[0]),
    .x0_1(b[0]),
    .y0_0(y0[0]),
    .y0_1(y1[0]),
    .x1_0(a[1]),
    .x1_1(b[1]),
    .y1_0(y0[1]),
    .y1_1(y1[1]),
    .x2_0(a[2]),
    .x2_1(b[2]),
    .y2_0(y0[2]),
    .y2_1(y1[2]),
    .x3_0(a[3]),
    .x3_1(b[3]),
    .y3_0(y0[3]),
    .y3_1(y1[3]),
    .rand_1(r[0]),
    .rand_2(r[1]),
    .rand_3(r[2]),
    .rand_4(r[3]),
    .rand_5(r[4]),
    .rand_6(r[5]),
    .rand_7(r[6]),
    .rand_8(r[7]),
    .rand_9(r[8]),
    .rand_10(r[9]),
    .rand_11(r[10]),
    .rand_12(r[11]),
    .rand_13(r[12]),
    .rand_14(r[13]),
    .rand_15(r[14]),
    .rand_16(r[15]),
    .rand_17(r[16]),
    .rand_18(r[17]),
    .rand_19(r[18]),
    .rand_20(r[19]),
    .rand_21(r[20]),
    .rand_22(r[21]),
    .rand_23(r[22]));
  function [3:0] present(input [3:0] x);
    case(x)
      0:present=12;1:present=5;2:present=6;3:present=11;
      4:present=9;5:present=0;6:present=10;7:present=13;
      8:present=3;9:present=14;10:present=15;11:present=8;
      12:present=4;13:present=7;14:present=1;15:present=2;
    endcase
  endfunction
  integer s,m,q,failures=0,checks=0;
  initial begin
    for(s=0;s<16;s=s+1)
      for(m=0;m<16;m=m+1)
        for(q=0;q<64;q=q+1) begin
          @(negedge clk); a=m;b=s^m;r=(q*23'h32ad73)^23'h52c319;
          repeat(40) @(posedge clk);
          #1;
          checks=checks+1;
          if((y0^y1)!==present(s)) begin
            if(failures<12) $display("FAIL secret=%h mask=%h random=%h got=%h expected=%h",s,m,q,y0^y1,present(s));
            failures=failures+1;
          end
        end
    $display("STABLE_TRUTH checks=%0d failures=%0d",checks,failures);
    if(failures) $fatal(1,"HLS stable truth failure");
    $finish;
  end
endmodule
