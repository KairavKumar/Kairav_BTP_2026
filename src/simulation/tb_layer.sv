`timescale 1ns/1fs
module tb_layer;
  reg clk=0;
  always #1 clk=~clk;
  reg [63:0] x0=0,x1=0;
  reg [367:0] randomness=0;
  wire [63:0] y0,y1;
  masked_present_layer uut(.clk(clk),.x0(x0),.x1(x1),.randomness(randomness),.y0(y0),.y1(y1));
  string vectors;
  integer fd,manifest,fields,label,trace_id=0;
  reg [63:0] guard_x0,guard_x1,test_x0,test_x1,expected;
  reg [367:0] guard_r,test_r;
  realtime start_ns,end_ns;
  initial begin
    if (!$value$plusargs("VECTORS=%s",vectors)) $fatal(1,"Missing vectors");
    $dumpfile("activity.vcd");
    // Flattened netlist nets suffice for cell-pin annotation; avoid dumping
    // duplicated private cell-model/internal notifier signals.
    $dumpvars(1,uut);
    fd=$fopen(vectors,"r");
    if (!fd) $fatal(1,"Cannot read vectors");
    manifest=$fopen("trace_manifest.csv","w");
    $fdisplay(manifest,"trace_id,class,start_ns,end_ns,expected");
    repeat(40) @(posedge clk);
    #0.2;
    while (!$feof(fd)) begin
      fields=$fscanf(fd,"%d %h %h %h %h %h %h %h\n",label,guard_x0,guard_x1,guard_r,test_x0,test_x1,test_r,expected);
      if (fields!=8) $fatal(1,"Malformed trace %0d",trace_id);
      x0=guard_x0;x1=guard_x1;randomness=guard_r;
      repeat(40) @(posedge clk);
      #0.2;
      x0=test_x0;x1=test_x1;randomness=test_r;
      start_ns=$realtime;
      repeat(40) @(posedge clk);
      #0.2;
      end_ns=$realtime;
      if ((y0^y1)!==expected) $fatal(1,"Functional mismatch trace=%0d got=%h expected=%h",trace_id,y0^y1,expected);
      $fdisplay(manifest,"%0d,%0d,%.3f,%.3f,%h",trace_id,label,start_ns,end_ns,expected);
      trace_id=trace_id+1;
    end
    $fclose(fd);$fclose(manifest);
    $display("FUNCTIONAL_PASS traces=%0d",trace_id);
    $finish;
  end
endmodule
