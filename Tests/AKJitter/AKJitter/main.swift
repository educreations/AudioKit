//
//  main.swift
//  AudioKit
//
//  Auto-generated on 12/21/14.
//  Customized by Aurelius Prochazka on 12/21/14.
//
//  Copyright (c) 2014 Aurelius Prochazka. All rights reserved.
//

import Foundation

class Instrument : AKInstrument {

    override init() {
        super.init()

        let operation = AKJitter()
        connect(operation)
        
        let sine = AKOscillator()
        sine.frequency = operation.scaledBy(3000.ak)
        connect(sine)

        connect(AKAudioOutput(audioSource:sine))
    }
}

// Set Up
let instrument = Instrument()
AKOrchestra.addInstrument(instrument)
AKManager.sharedManager().isLogging = true
AKOrchestra.testForDuration(10)

instrument.play()

while(AKManager.sharedManager().isRunning) {} //do nothing
println("Test complete!")
