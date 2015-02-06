# flux

`flux` is a framework for flexible, high-performance computation.

## example

    #include <flux/flux.hh>

    using namespace flux;
    
    void test()
    {
      // All computation is performed within the scope of a graph
      
      Graph graph;
      
      // Create a domain for each entity type
      
      auto& instrument = graph.addDomain<string>(); // instruments have string keys
      auto& trade = graph.addDomain<int>();         // trades have integer keys
      
      // Fields live within domains and hold values of a single data type
      
      auto& lastPx = instrument.createField<double>();
      auto& usdRate = instrument.createField<double>();
      auto& adjHistClosePx = instrument.createField<double>();
      auto& sodPos = instrument.createField<long>();
    
      auto& cumQty = trade.createField<unsigned>();
      auto& avgPx = trade.createField<double>();
      auto& tradeInstrumentId = trade.createField<string>();    

      // Specify any relationships between domains
    
      graph.addRelation(tradeInstrumentId, instrument);
        
      // Create fields that represent computed/derived values
      
      auto& tradeReturn = trade.compute<double>(
        { &cumQty, &lastPx, &avgPx, &usdRate },
        [&](const Params& vals)
        {
          return vals(cumQty) * (vals(lastPx) - vals(avgPx)) * vals(usdRate);
        });

      // Register for notification of value updates

      tradeReturn.subscribe([&](int tradeId, double return)
      {
        cout << "Trade " << tradeId << " has return " << return << endl;
      });

      // Set values
      
      int tradeId = 1;
      cumQty.setValue(1, 100);
      avgPx.setValue(1, 12.3);
      lastPx.setValue(1, 12.5);
      usdRate.setValue(1, 3.21);
      
      // Trigger computation
      
      graph.compute();
      
      // Trigger publication
      
      graph.publish();
    }
