module cost_aware_tree(input clk, input a, input b, output y);
  wire qa, qb, delayed, z;
  DFF_X1 r0 (.D(a), .CK(clk), .Q(qa), .QN());
  DFF_X1 r1 (.D(b), .CK(clk), .Q(qb), .QN());
  BUF_X1 data_delay (.A(qa), .Z(delayed));
  XOR2_X1 meeting (.A(delayed), .B(qb), .Z(z));
  DFF_X1 r2 (.D(z), .CK(clk), .Q(y), .QN());
endmodule
