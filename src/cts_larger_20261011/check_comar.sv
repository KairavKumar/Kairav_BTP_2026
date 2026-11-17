module check_comar;
  reg clk=0;
  always #5 clk=~clk;
  reg [3:0] a=0,b=0;
  reg [5:0] r=0;
  wire [3:0] y0,y1;
  sbox dut(clk,a[0],a[1],a[2],a[3],b[0],b[1],b[2],b[3],
    r[0],r[1],r[2],r[3],r[4],r[5],
    y0[0],y0[1],y0[2],y0[3],y1[0],y1[1],y1[2],y1[3]);
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
          @(negedge clk); a=m;b=s^m;r=q;
          repeat(12) @(posedge clk);
          #1;
          checks=checks+1;
          if((y0^y1)!==present(s)) begin
            if(failures<12) $display("FAIL secret=%h mask=%h random=%h got=%h expected=%h",s,m,q,y0^y1,present(s));
            failures=failures+1;
          end
        end
    $display("STABLE_TRUTH checks=%0d failures=%0d",checks,failures);
    if(failures) $fatal(1,"COMAR stable truth failure");
    $finish;
  end
endmodule
