//
//  LibSoftU2FTests.swift
//  LibSoftU2FTests
//
//  Created by Benjamin P Toews on 2/9/17.
//  Copyright © 2017 GitHub. All rights reserved.
//

import XCTest

let U2FHID_PING: UInt8 = 0x81
let U2FHID_INIT: UInt8 = 0x86

class LibSoftU2FTests: XCTestCase {
    var ctx: OpaquePointer? = nil
    var devs: UnsafeMutablePointer<u2fh_devs>? = nil
    
    var device: u2fdevice? {
        var dev = devs?.pointee.first
        
        while dev != nil {
            if dev!.pointee.id == 0 {
                return dev!.pointee
            } else {
                dev = dev!.pointee.next
            }
        }
        
        return nil
    }
    
    override func setUp() {
        super.setUp()
        
        ctx = softu2f_init(softu2f_init_flags(rawValue: 0))
        if ctx == nil {
            XCTFail("Couldn't initialize libsoftu2f")
            return
        }
        
        let _ = Thread {
            softu2f_run(self.ctx)
        }

        var rc = u2fh_global_init(u2fh_initflags(rawValue: 0))
        if rc != U2FH_OK {
            XCTFail("Coulnd't initialize libu2f-host")
            return
        }
        
        rc = u2fh_devs_init(&devs)
        if rc != U2FH_OK || devs == nil {
            XCTFail("Coulnd't initialize libu2f-host devices")
            return
        }
        
        var maxIdx: UInt32 = 0xABCDEF
        while u2fh_devs_discover(devs, &maxIdx) == U2FH_NO_U2F_DEVICE {
            u2fh_devs_done(devs)
            devs = nil
            sleep(1)
            
            rc = u2fh_devs_init(&devs)
            if rc != U2FH_OK || devs == nil {
                XCTFail("Coulnd't initialize libu2f-host devices")
                return
            }
        }
        
        if rc != U2FH_OK || maxIdx != 0 {
            XCTFail("Error discovering softu2f device")
            return
        }
    }
    
    override func tearDown() {
        u2fh_devs_done(devs)
        u2fh_global_done()
        if ctx != nil {
            softu2f_shutdown(ctx)
            sleep(1)
            softu2f_deinit(ctx)
        }
        super.tearDown()
    }

    func testInit() {
        guard var dev = device else {
            XCTFail("Error discovering softu2f device")
            return
        }
        
        dev.cid = CID_BROADCAST

        var req: [UInt8] = [0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88]
        let reqLen = UInt16(req.count)
        var respBytes = [UInt8](repeating: 0x00, count: 1024)
        var respLen = respBytes.count
        
        let rc = u2fh_sendrecv(devs, dev.id, U2FHID_INIT, &req, reqLen, &respBytes, &respLen)
        
        if rc != U2FH_OK {
            XCTFail("Error calling u2fh_sendrecv")
            return
        }

        XCTAssertEqual(respLen, MemoryLayout<U2FHID_INIT_RESP>.size)
        
        let resp: U2FHID_INIT_RESP = Data(bytes: respBytes).withUnsafeBytes { respPtr in
            return respPtr.pointee
        }
        
        XCTAssertEqual(req, [resp.nonce.0, resp.nonce.1, resp.nonce.2, resp.nonce.3, resp.nonce.4, resp.nonce.5, resp.nonce.6, resp.nonce.7])
        XCTAssertEqual(resp.capFlags, UInt8(CAPFLAG_WINK))
    }
    
    func testPing() {
        guard let dev = device else {
            XCTFail("Error discovering softu2f device")
            return
        }
        
        var req: [UInt8] = [0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88]
        let reqLen = req.count
        var respBytes = [UInt8](repeating: 0x00, count: 1024)
        var respLen = respBytes.count
        
        let rc = u2fh_sendrecv(devs, dev.id, U2FHID_INIT, &req, UInt16(reqLen), &respBytes, &respLen)
        
        if rc != U2FH_OK {
            XCTFail("Error calling u2fh_sendrecv")
            return
        }
        
        XCTAssertEqual(respLen, reqLen)
        XCTAssertEqual(respBytes[0..<reqLen], req[0..<reqLen])
    }
    
    func testFragmentation() {
        guard let dev = device else {
            XCTFail("Error discovering softu2f device")
            return
        }
        
        var req: [UInt8] = [
            0x4a, 0x5e, 0xd7, 0xc5, 0x68, 0xf3, 0xa3, 0x09, 0xd5, 0xf5, 0x02, 0xef, 0xb9, 0x2e, 0xd0, 0xb6,
            0x86, 0x18, 0xa4, 0xc4, 0xfc, 0xd3, 0xd0, 0x56, 0x32, 0x2c, 0x96, 0x35, 0xb5, 0xdf, 0xd9, 0x2d,
            0xbb, 0x05, 0xd9, 0xd4, 0xec, 0x14, 0x96, 0xea, 0x8e, 0x14, 0x34, 0xad, 0xec, 0x7f, 0x4d, 0x30,
            0x69, 0x22, 0x60, 0x82, 0x43, 0x85, 0x66, 0xa3, 0x5d, 0x3f, 0x3c, 0xe2, 0x59, 0x0e, 0x8f, 0x08,
            0x18, 0x98, 0xb6, 0x07, 0xd9, 0x62, 0xcc, 0xbd, 0xc0, 0x1e, 0x41, 0x8c, 0x17, 0xd1, 0x94, 0x3d,
            0x02, 0x29, 0x2e, 0xc6, 0x60, 0x4a, 0x99, 0x3f, 0xd6, 0xd1, 0xfd, 0x45, 0x43, 0xb5, 0x4e, 0xd7,
            0x93, 0xbf, 0xd5, 0x94, 0x60, 0x0e, 0x67, 0xc6, 0x1a, 0xaf, 0x83, 0x48, 0x8d, 0xc9, 0x01, 0xca,
            0x10, 0x67, 0x41, 0x1f, 0xeb, 0x88, 0x36, 0xb8, 0xfb, 0xfe, 0x8b, 0x0f, 0x5f, 0x86, 0xdd, 0x27,
            0xd6, 0xff, 0xbd, 0x8d, 0x56, 0x58, 0x7b, 0x77, 0x2d, 0x6e, 0x4a, 0x34, 0xd3, 0x9d, 0x74, 0x2c,
            0xc7, 0xf2, 0x60, 0x28, 0x8d, 0x0b, 0xa2, 0x94, 0xb5, 0xff, 0x34, 0x13, 0x6c, 0xea, 0xd7, 0x9d,
            0x98, 0xe2, 0x05, 0xa0, 0xb6, 0x68, 0xb6, 0x74, 0x0f, 0x22, 0xdb, 0x36, 0xb2, 0x07, 0x94, 0x81,
            0x7f, 0xa0, 0x6c, 0x3e, 0xa2, 0x78, 0x6e, 0xd1, 0x13, 0xcd, 0x75, 0x37, 0x8e, 0x91, 0x23, 0x47,
            0xb1, 0x90, 0xd3, 0x56, 0xea, 0x6e, 0x8f, 0x4c, 0x47, 0x38, 0x54, 0x38, 0x43, 0x54, 0xb5, 0x76,
            0xa1, 0xaa, 0x95, 0x39, 0x85, 0xdd, 0x55, 0xca, 0x96, 0x72, 0x66, 0xe6, 0xb0, 0x28, 0xe0, 0xd9,
            0x4b, 0xb0, 0x26, 0x90, 0x2f, 0x6d, 0xb6, 0x3b, 0xec, 0xaa, 0x2e, 0xb1, 0x04, 0x45, 0x95, 0xb4,
            0x09, 0xa5, 0x5d, 0x4c, 0x65, 0x79, 0x92, 0x0d, 0xff, 0xde, 0x74, 0xc6, 0x3c, 0xd0, 0x29, 0xff,
            0xf5, 0xde, 0xd7, 0x09, 0x1f, 0x7e, 0x9b, 0x1e, 0x8c, 0xa7, 0x25, 0x76, 0x8c, 0xfd, 0x73, 0xce,
            0x66, 0x71, 0x60, 0x29, 0x2a, 0xd0, 0xb2, 0x72, 0xb5, 0xe7, 0x29, 0x4c, 0x9f, 0x34, 0x70, 0x49,
            0x20, 0x34, 0x89, 0x7b, 0xb6, 0x2f, 0xf1, 0x96, 0x9d, 0x9b, 0x5c, 0x43, 0x2d, 0x02, 0x56, 0xf4,
            0xba, 0xb6, 0xf2, 0x1d, 0x3a, 0x1f, 0xe2, 0x92, 0x27, 0x68, 0xfd, 0x80, 0x22, 0xf0, 0x1e, 0x1d,
            0x12, 0x48, 0xf4, 0x59, 0xce, 0x66, 0x3b, 0x94, 0xf9, 0x1d, 0x5e, 0xe7, 0x4d, 0xcc, 0xe7, 0x35,
            0xe1, 0x84, 0x1b, 0xcc, 0xba, 0x5a, 0xc1, 0x20, 0xd7, 0x18, 0x3f, 0xa0, 0xc4, 0x91, 0x7b, 0x32,
            0x3b, 0xfe, 0x9e, 0x70, 0x8c, 0x3a, 0x69, 0x86, 0x95, 0x07, 0x75, 0x79, 0xe1, 0x21, 0x51, 0xcb,
            0x21, 0xc4, 0x3c, 0x84, 0x3a, 0xb4, 0x6c, 0xf0, 0x02, 0xd3, 0xfd, 0xa3, 0x9e, 0x44, 0xb6, 0x1f,
            0xd7, 0x70, 0x0a, 0xd0, 0x26, 0xf3, 0x95, 0xc8, 0x4d, 0xcc, 0x4d, 0xc3, 0xaa, 0x92, 0xf6, 0x53,
            0x5e, 0xa0, 0xac, 0xae, 0xb7, 0xda, 0x7a, 0x97, 0xa8, 0x5f, 0x94, 0x4a, 0xc9, 0x0d, 0x6e, 0xe1,
            0xaf, 0x63, 0x46, 0x41, 0xe1, 0x26, 0x20, 0x7e, 0xc4, 0xa9, 0xa2, 0xfb, 0x12, 0x33, 0x56, 0x69,
            0x36, 0xc6, 0xab, 0x9b, 0x53, 0x93, 0xf3, 0x89, 0x16, 0xff, 0x38, 0xf3, 0xc4, 0x51, 0x6f, 0x84,
            0x2f, 0x1c, 0xbd, 0x6e, 0x39, 0xeb, 0xb6, 0xb9, 0x9c, 0x51, 0x90, 0xd1, 0x59, 0x51, 0x61, 0x53,
            0x3d, 0x3e, 0x22, 0x57, 0x0b, 0x5b, 0xe0, 0x71, 0xc6, 0x93, 0x9e, 0x04, 0x8c, 0x97, 0xbf, 0xea,
            0x5d, 0xa3, 0xca, 0x1b, 0x52, 0x41, 0x31, 0xab, 0x12, 0xb9, 0xdd, 0xc9, 0x22, 0xa0, 0x4b, 0xbe,
            0x7e, 0xbc, 0xc9, 0x5b
        ]
        
        let reqLen = req.count
        var respBytes = [UInt8](repeating: 0x00, count: 1024)
        var respLen = respBytes.count

        let rc = u2fh_sendrecv(devs, dev.id, U2FHID_INIT, &req, UInt16(reqLen), &respBytes, &respLen)
        
        if rc != U2FH_OK {
            XCTFail("Error calling u2fh_sendrecv")
            return
        }
        
        XCTAssertEqual(respLen, reqLen)
        XCTAssertEqual(respBytes[0..<reqLen], req[0..<reqLen])
        
    }
}
